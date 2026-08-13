/*
 *   -H                 print header
 *   -q                 add summary block at the end
 *   --range A-B   heap pages [A,B]
 *   --heap-page N      dump only: status of one heap page
 *   --extra            print one line per heap page
 *   --only-not-visible dump: only print ranges where ALL_VISIBLE is not set
 *   --only-not-frozen  dump: only print ranges where ALL_FROZEN is not set
 *   --notF             short cut for --only-not-frozen
 *   --notV             short cut for --only-not-visible
 *   --only-changed     diff: only print ranges whose status changed
 *
 */
#include "postgres.h"

#if PG_VERSION_NUM >= 130000
#include "access/visibilitymapdefs.h"
#endif
#include "access/visibilitymap.h"

#include "storage/bufpage.h"

#include "pg_vm.h"

#define MAP_SIZE ((int)(BLCKSZ - MAXALIGN(SizeOfPageHeaderData)))
#define HEAPBLOCKS_PER_BYTE (8 / BITS_PER_HEAPBLOCK)	/* 4 */
#define HEAPBLOCKS_PER_PAGE (MAP_SIZE * HEAPBLOCKS_PER_BYTE)

#define VM_STATUS_OUT_OF_FILE (-1)
#define VM_STATUS_CORRUPT (-2)

/*
 * Help understand what was wrong during parse of the page header
 */
typedef enum
{
	HEADER_OK, HEADER_WRONG_PAGESIZE, HEADER_INVALID
}			HeaderStatus;

/*
 * VmResult - status code returned by the do_vm_dump()/do_vm_diff()
 */
typedef enum
{
	VM_FAIL = 0, VM_SUCCESS = 1
} VmResult;

/*
 * VmPageInfo - everything about one VM (visibility map) page.
 */
typedef struct
{
	long		page;			/* this page's index in the vm file */

	int			valid;			/* passed vm_header_status() checks */
	int			allzero;		/* uninitialized (all-zero) hole page */

	HeaderStatus header_status; /* result of header sanity checks */
	const char *invalid_reason; /* text reason if invalid */

	uint8		bitmap[MAP_SIZE];	/* per-page visibility bits */
	PageHeaderData header;		/* raw copy of the standard page header */
}			VmPageInfo;

/*
 * One run of heap pages sharing the same status
 */
typedef struct
{
	long		heap_start,
				heap_end;
	int			status;			/* the shared status across [heap_start,
								 * heap_end] */
}			VmRun;

typedef struct
{
	VmRun	   *items;
	long		count,
				cap;
}			VmRunVec;

/*
 * One run of heap pages where the old/new status pair stays equal;
 * used by the diff command.
 */
typedef struct
{
	long		heap_start,
				heap_end;
	int			old_status,
				new_status;		/* the shared (old,new) pair across the run */
}			VmDiffRun;

typedef struct
{
	VmDiffRun  *items;
	long		count,
				cap;
}			VmDiffRunVec;

/*
 * vm_page_is_all_zero - true if every byte of the page buffer is zero
 */
static int
vm_page_is_all_zero(const uint8 *buf)
{
	int			i;

	for (i = 0; i < BLCKSZ; i++)
	{
		if (buf[i] != 0)
		{
			return 0;
		}
	}
	return 1;
}

/*
 * vm_header_status - sanity-check a page header (size, version, and the
 * lower/upper/special offset ordering).
 */
static HeaderStatus vm_header_status(PageHeader page_header,
									 const char **reason)
{
	uint16		pagesize = page_header->pd_pagesize_version & 0xFF00;
	uint16		version = page_header->pd_pagesize_version & 0x00FF;

	if (pagesize != BLCKSZ)
	{
		*reason = "page size in header does not match BLCKSZ this binary was "
			"built with";
		return HEADER_WRONG_PAGESIZE;
	}
	if (version == 0 || version > PG_PAGE_LAYOUT_VERSION)
	{
		*reason = "bad page layout version";
		return HEADER_INVALID;
	}
	if (page_header->pd_special > BLCKSZ)
	{
		*reason = "pd_special is larger than the page";
		return HEADER_INVALID;
	}
	if (page_header->pd_lower > page_header->pd_upper)
	{
		*reason = "pd_lower is greater than pd_upper";
		return HEADER_INVALID;
	}
	if (page_header->pd_upper > page_header->pd_special)
	{
		*reason = "pd_upper is greater than pd_special";
		return HEADER_INVALID;
	}
	*reason = NULL;
	return HEADER_OK;
}

/*
 * fill_vm_page_info - classify all-zero/valid status, validate the header,
 * and copy the bitmap for one VM page.
 */
static void
fill_vm_page_info(const uint8 *buf, long page_index,
				  VmPageInfo * page_info)
{
	PageHeader	page_header;
	const char *reason = NULL;

	page_header = (PageHeader) buf;

	page_info->page = page_index;
	page_info->allzero = vm_page_is_all_zero(buf);
	page_info->header_status =
		page_info->allzero ? HEADER_OK : vm_header_status(page_header, &reason);
	page_info->invalid_reason = reason;
	page_info->valid =
		!page_info->allzero && page_info->header_status == HEADER_OK;
	page_info->header = *page_header;

	if (page_info->allzero || page_info->valid)
	{
		memcpy(page_info->bitmap, PageGetContents((Page) buf), MAP_SIZE);
	}
	else
	{
		memset(page_info->bitmap, 0, MAP_SIZE);
	}
}

/*
 * load_vm - read a VM file into memory page by page. Returns an array of
 * VmPageInfo.
 */
static VmPageInfo * load_vm(const char *path, long *out_total_pages)
{
	FILE	   *f;
	long		filesize;
	long		total_pages;
	VmPageInfo *pages;
	uint8		buf[BLCKSZ];
	long		page_index;

	f = fopen(path, "rb");
	if (!f)
	{
		perror(path);
		return NULL;
	}

	fseek(f, 0, SEEK_END);
	filesize = ftell(f);
	fseek(f, 0, SEEK_SET);
	total_pages = filesize / BLCKSZ;

	if (total_pages <= 0)
	{
		fprintf(stderr, "not a valid vm file (empty or truncated): %s\n", path);
		fclose(f);
		return NULL;
	}

	pages = calloc(total_pages, sizeof(VmPageInfo));
	if (!pages)
	{
		fprintf(stderr, "%s: out of memory allocating %ld page(s)\n", path,
				total_pages);
		fclose(f);
		return NULL;
	}

	for (page_index = 0; page_index < total_pages; page_index++)
	{
		if (fread(buf, 1, BLCKSZ, f) != (size_t) BLCKSZ)
		{
			fprintf(stderr,
					"warning: short read at page %ld, treating as zero\n",
					page_index);
			memset(buf, 0, BLCKSZ);
		}
		fill_vm_page_info(buf, page_index, &pages[page_index]);
	}
	fclose(f);
	*out_total_pages = total_pages;
	return pages;
}

/*
 * heap_page_status - look up the visibility-map status bits for one heap
 * page.
 */
static int
heap_page_status(const VmPageInfo * pages, long total_pages,
				 long heap_page)
{
	long		vm_page;
	const		VmPageInfo *page_info;
	long		offset;
	long		byte_idx;
	int			bit_shift;

	vm_page = heap_page / HEAPBLOCKS_PER_PAGE;
	if (vm_page >= total_pages)
	{
		return VM_STATUS_OUT_OF_FILE;
	}

	page_info = &pages[vm_page];
	if (!page_info->allzero && !page_info->valid)
	{
		return VM_STATUS_CORRUPT;
	}

	offset = heap_page % HEAPBLOCKS_PER_PAGE;
	byte_idx = offset / HEAPBLOCKS_PER_BYTE;
	bit_shift = (int) (offset % HEAPBLOCKS_PER_BYTE) * BITS_PER_HEAPBLOCK;
	return (page_info->bitmap[byte_idx] >> bit_shift) &
		VISIBILITYMAP_VALID_BITS;
}

/*
 * vmrun_push - append one run to a VmRunVec, growing it if needed.
 */
static void
vmrun_push(VmRunVec * vec, long start, long end, int status)
{
	if (vec->count == vec->cap)
	{
		vec->cap = vec->cap ? vec->cap * 2 : 16;
		vec->items = realloc(vec->items, vec->cap * sizeof(VmRun));
	}
	vec->items[vec->count++] = (VmRun)
	{
		start, end, status
	};
}

/*
 * compute_compressed - run-length compress heap page statuses over
 * [from, to] into a VmRunVec.
 */
static VmRunVec compute_compressed(const VmPageInfo * pages, long total_pages,
								   long from, long to)
{
	VmRunVec	compressed = {0};
	long		run_start;
	int			run_status;
	long		hp;

	if (to < from)
	{
		return compressed;
	}

	run_start = from;
	run_status = heap_page_status(pages, total_pages, from);

	for (hp = from + 1; hp <= to; hp++)
	{
		int			status = heap_page_status(pages, total_pages, hp);

		if (status != run_status)
		{
			vmrun_push(&compressed, run_start, hp - 1, run_status);
			run_start = hp;
			run_status = status;
		}
	}
	vmrun_push(&compressed, run_start, to, run_status);
	return compressed;
}

/*
 * vmdiffrun_push - append one run to a VmDiffRunVec, growing it if needed.
 */
static void
vmdiffrun_push(VmDiffRunVec * vec, long start, long end,
			   int old_status, int new_status)
{
	if (vec->count == vec->cap)
	{
		vec->cap = vec->cap ? vec->cap * 2 : 16;
		vec->items = realloc(vec->items, vec->cap * sizeof(VmDiffRun));
	}
	vec->items[vec->count++] = (VmDiffRun)
	{
		start, end, old_status, new_status
	};
}

/*
 * compute_diff_compressed - like compute_compressed, but over a pair of
 * VM files (old/new), used by the diff command.
 */
static VmDiffRunVec compute_diff_compressed(const VmPageInfo * old_pages,
											long total_old,
											const VmPageInfo * new_pages,
											long total_new, long from,
											long to)
{
	VmDiffRunVec compressed = {0};
	long		run_start;
	int			old_run_status;
	int			new_run_status;
	long		hp;

	if (to < from)
	{
		return compressed;
	}

	run_start = from;
	old_run_status = heap_page_status(old_pages, total_old, from);
	new_run_status = heap_page_status(new_pages, total_new, from);

	for (hp = from + 1; hp <= to; hp++)
	{
		int			old_status = heap_page_status(old_pages, total_old, hp);
		int			new_status = heap_page_status(new_pages, total_new, hp);

		if (old_status != old_run_status || new_status != new_run_status)
		{
			vmdiffrun_push(&compressed, run_start, hp - 1, old_run_status,
						   new_run_status);
			run_start = hp;
			old_run_status = old_status;
			new_run_status = new_status;
		}
	}
	vmdiffrun_push(&compressed, run_start, to, old_run_status, new_run_status);
	return compressed;
}

/*
 * status_label - human-readable label for a heap page's VM status.
 */
static const char *
status_label(int status)
{
	switch (status)
	{
		case 0:
			return "none";
		case VISIBILITYMAP_ALL_VISIBLE:
			return "visible";
		case VISIBILITYMAP_ALL_FROZEN:
			return "frozen-only(!)";
		case VISIBILITYMAP_ALL_VISIBLE | VISIBILITYMAP_ALL_FROZEN:
			return "visible+frozen";
		case VM_STATUS_OUT_OF_FILE:
			return "out-of-file";
		case VM_STATUS_CORRUPT:
			return "CORRUPT-HEADER";
		default:
			return "?";
	}
}

/*
 * status_has_visible - true if a status value carries ALL_VISIBLE
 */
static int
status_has_visible(int status)
{
	return status > 0 && (status & VISIBILITYMAP_ALL_VISIBLE);
}

/*
 * status_has_frozen - true if a status value carries ALL_FROZEN
 */
static int
status_has_frozen(int status)
{
	return status > 0 && (status & VISIBILITYMAP_ALL_FROZEN);
}

/*
 * print_header_line - print one page's raw header fields
 */
static void
print_header_line(FILE *out, const VmPageInfo * page_info)
{
	PageHeaderData header;
	unsigned long long lsn = 0;

	header = page_info->header;

#if PG_VERSION_NUM >= 190000
		lsn = (unsigned long long) PageXLogRecPtrGet(&header.pd_lsn);
#elif PG_VERSION_NUM >= 140000
		lsn = (unsigned long long) PageXLogRecPtrGet(header.pd_lsn);
#else 
		lsn=((unsigned long long) header.pd_lsn.xlogid << 32) | header.pd_lsn.xrecoff;
#endif

	fprintf(
			out,
			"vm page %ld: lsn=%llX checksum=%u flags=0x%x lower=%u upper=%u "
			"special=%u\n",
			page_info->page, lsn,
			header.pd_checksum, header.pd_flags, header.pd_lower,
			header.pd_upper, header.pd_special);
}

/*
 * print_page_headers - print the "-H" per-physical-page header inventory.
 */
static void
print_page_headers(FILE *out, const VmPageInfo * pages,
				   long total_pages)
{
	long		page_index;

	fprintf(out, "\n-- physical page headers (-H) --\n");

	for (page_index = 0; page_index < total_pages; page_index++)
	{
		const		VmPageInfo *page_info = &pages[page_index];

		if (page_info->allzero)
		{
			fprintf(out, "vm page %ld: empty (all-zero)\n", page_index);
			continue;
		}
		if (page_info->header_status == HEADER_WRONG_PAGESIZE)
		{
			fprintf(out,
					"vm page %ld: -- WRONG PAGE SIZE, rebuild pg_vm with the "
					"server's --with-blocksize\n",
					page_index);
			continue;
		}
		if (page_info->header_status == HEADER_INVALID)
		{
			fprintf(out, "vm page %ld: header valid: no (%s)\n", page_index,
					page_info->invalid_reason);
			continue;
		}

		print_header_line(out, page_info);
	}
}

/*
 * print_compressed - print the "-s"-style compressed page status
 * ranges for the dump command.
 */
static void
print_compressed(FILE *out, const VmRunVec * compressed,
				 const VmOptions * options)
{
	long		i;

	for (i = 0; i < compressed->count; i++)
	{
		const		VmRun *r = &compressed->items[i];

		if (options->only_not_visible && status_has_visible(r->status))
		{
			continue;
		}
		if (options->only_not_frozen && status_has_frozen(r->status))
		{
			continue;
		}

		if (r->heap_start == r->heap_end)
		{
			fprintf(out, "heap page %8ld           : %s\n", r->heap_start,
					status_label(r->status));
		}
		else
		{
			fprintf(out, "heap pages %8ld-%-8ld: %s (%ld page(s))\n",
					r->heap_start, r->heap_end, status_label(r->status),
					r->heap_end - r->heap_start + 1);
		}
	}
}

/*
 * print_expanded - print one line per heap page (the "--extra" dump view).
 */
static void
print_expanded(FILE *out, const VmPageInfo * pages, long total_pages,
			   long from, long to, const VmOptions * options)
{
	long		hp;

	for (hp = from; hp <= to; hp++)
	{
		int			status = heap_page_status(pages, total_pages, hp);

		if (options->only_not_visible && status_has_visible(status))
		{
			continue;
		}
		if (options->only_not_frozen && status_has_frozen(status))
		{
			continue;
		}
		fprintf(out, "heap page %8ld: %s\n", hp, status_label(status));
	}
}

/*
 * print_diff_compressed - print the compressed old->new status ranges for
 * the diff command.
 */
static void
print_diff_compressed(FILE *out, const VmDiffRunVec * compressed,
					  const VmOptions * options)
{
	long		i;

	for (i = 0; i < compressed->count; i++)
	{
		const		VmDiffRun *r = &compressed->items[i];
		const char *tag;

		if (options->only_changed && r->old_status == r->new_status)
		{
			continue;
		}
		tag = r->old_status == r->new_status ? "same" : "CHANGED";

		if (r->heap_start == r->heap_end)
		{
			fprintf(out, "heap page %8ld           : %s -> %s  [%s]\n",
					r->heap_start, status_label(r->old_status),
					status_label(r->new_status), tag);
		}
		else
		{
			fprintf(out,
					"heap pages %8ld-%-8ld: %s -> %s  [%s] (%ld page(s))\n",
					r->heap_start, r->heap_end, status_label(r->old_status),
					status_label(r->new_status), tag,
					r->heap_end - r->heap_start + 1);
		}
	}
}

/*
 * print_expanded_diff - print one old->new line per heap page (the
 * "--extra" diff view).
 */
static void
print_expanded_diff(FILE *out, const VmPageInfo * old_pages,
					long total_old, const VmPageInfo * new_pages,
					long total_new, long from, long to,
					const VmOptions * options)
{
	long		hp;

	for (hp = from; hp <= to; hp++)
	{
		int			old_status = heap_page_status(old_pages, total_old, hp);
		int			new_status = heap_page_status(new_pages, total_new, hp);

		if (options->only_changed && old_status == new_status)
		{
			continue;
		}
		fprintf(out, "heap page %8ld: %s -> %s  [%s]\n", hp,
				status_label(old_status), status_label(new_status),
				old_status == new_status ? "same" : "CHANGED");
	}
}

/*
 * default_heap_to - the last valid heap page index for a VM file with
 * total_pages physical pages, used when --range is not given.
 */
static long
default_heap_to(long total_pages)
{
	return total_pages * (long) HEAPBLOCKS_PER_PAGE - 1;
}

/*
 * resolve_heap_range - work out the [from, to] page range to process,
 * honoring --range if it was given.
 */
static void
resolve_heap_range(const VmOptions * options, long total_pages,
				   long *from, long *to)
{
	*from = options->has_range ? options->range_lo : 0;
	*to = options->has_range ? options->range_hi
		: default_heap_to(total_pages);
}

/*
 * do_vm_dump_heap_page - implement the "--page N" lookup mode: print
 * the status of a single heap page and nothing else.
 */
static void
do_vm_dump_heap_page(FILE *out, const char *in_path,
					 const VmPageInfo * pages, long total_pages,
					 const VmOptions * options)
{
	int			status;

	status = heap_page_status(pages, total_pages, options->heap_page_query);

	fprintf(out, "=== pg_vm dump: %s (--page %ld lookup) ===\n", in_path,
			options->heap_page_query);
	fprintf(out, "heap page %ld: %s\n", options->heap_page_query,
			status_label(status));
}

/*
 * print_dump_summary - print the "-q" SUMMARY block for the dump command.
 */
static void
print_dump_summary(FILE *out, const VmRunVec * compressed, long from,
				   long to)
{
	long		visible = 0;
	long		frozen = 0;
	long		corrupt_runs = 0;
	long		i;

	for (i = 0; i < compressed->count; i++)
	{
		const		VmRun *r = &compressed->items[i];
		long		n = r->heap_end - r->heap_start + 1;

		if (status_has_visible(r->status))
		{
			visible += n;
		}
		if (status_has_frozen(r->status))
		{
			frozen += n;
		}
		if (r->status == VM_STATUS_CORRUPT)
		{
			corrupt_runs++;
		}
	}

	fprintf(out,
			"\n--------------------------------------------------------------"
			"\nSUMMARY\n--------------------------------------------------"
			"------------\n");
	fprintf(out, "heap pages in range: %ld\n", to - from + 1);
	fprintf(out, "all-visible: %ld  all-frozen: %ld  error page(s): %ld\n",
			visible, frozen, corrupt_runs);
}

/*
 * do_vm_dump_range - print the (compressed or expanded) page status
 * for [from, to], plus an optional summary.
 */
static void
do_vm_dump_range(FILE *out, const VmPageInfo * pages,
				 long total_pages, long from, long to,
				 const VmOptions * options)
{
	fprintf(out, "\n-- heap page status%s --\n",
			options->expand ? " (expanded)" : " (compressed)");

	if (options->expand)
	{
		print_expanded(out, pages, total_pages, from, to, options);
		return;
	}

	{
		VmRunVec	compressed = compute_compressed(pages, total_pages, from, to);

		print_compressed(out, &compressed, options);
		if (options->stats)
		{
			print_dump_summary(out, &compressed, from, to);
		}
		free(compressed.items);
	}
}

/*
 * do_vm_dump - implement the "dump" command: load the VM file, then either
 * answer a single --page query or print the full (compressed or
 * expanded) page status listing plus an optional summary (-q).
 */
static VmResult do_vm_dump(const char *in_path, const char *out_path,
						   const VmOptions * options)
{
	long		total_pages;
	VmPageInfo *pages;
	FILE	   *out;
	long		from,
				to;

	pages = load_vm(in_path, &total_pages);
	if (!pages)
	{
		return VM_FAIL;
	}

	out = fopen(out_path, "w");
	if (!out)
	{
		perror(out_path);
		free(pages);
		return VM_FAIL;
	}

	if (options->has_heap_page)
	{
		do_vm_dump_heap_page(out, in_path, pages, total_pages, options);
		fclose(out);
		free(pages);
		fprintf(stderr, "wrote %s\n", out_path);
		return VM_SUCCESS;
	}

	resolve_heap_range(options, total_pages, &from, &to);

	fprintf(out, "=== pg_vm dump: %s ===\n", in_path);
	fprintf(out,
			"file size: %ld bytes, total pages: %ld, "
			"%d bytes/page\n",
			total_pages * BLCKSZ, total_pages, BLCKSZ);
	fprintf(out, "heap page range covered: [%ld, %ld]\n", from, to);
	fprintf(out, "\n");

	if (options->show_headers)
	{
		print_page_headers(out, pages, total_pages);
	}

	do_vm_dump_range(out, pages, total_pages, from, to, options);

	fclose(out);
	free(pages);
	fprintf(stderr, "wrote %s\n", out_path);
	return VM_SUCCESS;
}

/*
 * print_diff_summary - print the "-q" SUMMARY block for the diff command.
 */
static void
print_diff_summary(FILE *out, const VmDiffRunVec * compressed,
				   long from, long to)
{
	long		changed_pages = 0;
	long		gained_visible = 0;
	long		lost_visible = 0;
	long		gained_frozen = 0;
	long		lost_frozen = 0;
	long		i;

	for (i = 0; i < compressed->count; i++)
	{
		const		VmDiffRun *r = &compressed->items[i];
		long		n = r->heap_end - r->heap_start + 1;
		int			old_visible,
					new_visible,
					old_frozen,
					new_frozen;

		if (r->old_status == r->new_status)
		{
			continue;
		}
		changed_pages += n;

		old_visible = status_has_visible(r->old_status);
		new_visible = status_has_visible(r->new_status);
		old_frozen = status_has_frozen(r->old_status);
		new_frozen = status_has_frozen(r->new_status);

		if (!old_visible && new_visible)
		{
			gained_visible += n;
		}
		if (old_visible && !new_visible)
		{
			lost_visible += n;
		}
		if (!old_frozen && new_frozen)
		{
			gained_frozen += n;
		}
		if (old_frozen && !new_frozen)
		{
			lost_frozen += n;
		}
	}

	fprintf(out,
			"\n--------------------------------------------------------------"
			"\nSUMMARY\n--------------------------------------------------"
			"------------\n");
	fprintf(out, "heap pages in range: %ld, compressed into %ld run(s)\n",
			to - from + 1, compressed->count);
	fprintf(out, "changed heap pages: %ld\n", changed_pages);
	fprintf(out, "+ all-visible: %ld  - all-visible: %ld\n", gained_visible,
			lost_visible);
	fprintf(out, "+ all-frozen: %ld  - all-frozen: %ld\n", gained_frozen,
			lost_frozen);
}

/*
 * do_vm_diff_range - print the (compressed or expanded) old->new status
 * for [from, to], plus an optional summary.
 */
static void
do_vm_diff_range(FILE *out, const VmPageInfo * old_pages,
				 long total_old, const VmPageInfo * new_pages,
				 long total_new, long from, long to,
				 const VmOptions * options)
{
	if (options->expand)
	{
		print_expanded_diff(out, old_pages, total_old, new_pages, total_new,
							from, to, options);
		return;
	}

	{
		VmDiffRunVec compressed = compute_diff_compressed(
														  old_pages, total_old, new_pages, total_new, from, to);

		print_diff_compressed(out, &compressed, options);
		if (options->stats)
		{
			print_diff_summary(out, &compressed, from, to);
		}
		free(compressed.items);
	}
}

/*
 * do_vm_diff - implement the "diff" command: load both VM files, then
 * print the (compressed or expanded) old->new page status for the
 * requested range plus an optional summary (-q).
 */
static VmResult do_vm_diff(const char *old_path, const char *new_path,
						   const char *out_path, const VmOptions * options)
{
	long		total_old,
				total_new;
	long		total_max;
	VmPageInfo *old_pages;
	VmPageInfo *new_pages;
	FILE	   *out;
	long		from,
				to;

	old_pages = load_vm(old_path, &total_old);
	if (!old_pages)
	{
		return VM_FAIL;
	}
	new_pages = load_vm(new_path, &total_new);
	if (!new_pages)
	{
		free(old_pages);
		return VM_FAIL;
	}

	out = fopen(out_path, "w");
	if (!out)
	{
		perror(out_path);
		free(old_pages);
		free(new_pages);
		return VM_FAIL;
	}

	total_max = total_old > total_new ? total_old : total_new;
	resolve_heap_range(options, total_max, &from, &to);

	fprintf(out,
			"=== pg_vm diff ===\nold: %s (%ld pages)\nnew: %s (%ld pages)\n"
			"heap page range covered: [%ld, %ld]\n\n",
			old_path, total_old, new_path, total_new, from, to);

	do_vm_diff_range(out, old_pages, total_old, new_pages, total_new, from, to,
					 options);

	fclose(out);
	free(old_pages);
	free(new_pages);
	fprintf(stderr, "wrote %s\n", out_path);
	return VM_SUCCESS;
}

/*
 * vm_usage - print command-line usage/help text to stderr.
 */
static void
vm_usage(const char *prog)
{
	fprintf(stderr,
			"usage:\n"
			"  %s dump [flags] <relfilenode_vm> <out.txt>\n"
			"  %s diff [flags] <old_vm> <new_vm> <out.txt>\n"
			"flags:\n"
			"  -H                  per-physical-page header inventory\n"
			"  -q                  add summary\n"
			"  --range A-B    process heap pages [A,B]\n"
			"  --heap-page N       dump only: status of one heap "
			"page\n"
			"  --extra             one line per heap page\n"
			"  --only-not-visible  dump: only ranges missing ALL_VISIBLE\n"
			"  --only-not-frozen   dump: only ranges missing ALL_FROZEN\n"
			"  --notF  --notV      short cut for the two flags above\n"
			"  --only-changed      diff: only ranges whose status changed\n",
			prog, prog);
}

/*
 * parse_vm_flags - parse command-line flags shared by "dump" and "diff"
 */
static void
parse_vm_flags(int argc, char **argv, int start, VmOptions * options,
			   char **pos, int *npos)
{
	int			i;

	*npos = 0;
	for (i = start; i < argc; i++)
	{
		const char *a = argv[i];

		if (strcmp(a, "--range") == 0 && i + 1 < argc)
		{
			long		lo,
						hi;

			if (sscanf(argv[++i], "%ld-%ld", &lo, &hi) == 2)
			{
				options->has_range = 1;
				options->range_lo = lo;
				options->range_hi = hi;
			}
			else
			{
				fprintf(stderr,
						"bad --range value %s, expected A-B (ignored)\n",
						argv[i]);
			}
		}
		else if (strcmp(a, "--heap-page") == 0 && i + 1 < argc)
		{
			options->has_heap_page = 1;
			options->heap_page_query = atol(argv[++i]);
		}
		else if (strcmp(a, "--extra") == 0)
		{
			options->expand = 1;
		}
		else if (strcmp(a, "--only-not-visible") == 0)
		{
			options->only_not_visible = 1;
		}
		else if (strcmp(a, "--notV") == 0)
		{
			options->only_not_visible = 1;
		}
		else if (strcmp(a, "--only-not-frozen") == 0)
		{
			options->only_not_frozen = 1;
		}
		else if (strcmp(a, "--notF") == 0)
		{
			options->only_not_frozen = 1;
		}
		else if (strcmp(a, "--only-changed") == 0)
		{
			options->only_changed = 1;
		}
		else if (a[0] == '-' && a[1] != '\0' && a[1] != '-')
		{
			const char *c;

			for (c = a + 1; *c; c++)
			{
				switch (*c)
				{
					case 'H':
						options->show_headers = 1;
						break;
					case 'q':
						options->stats = 1;
						break;
					default:
						fprintf(stderr, "unknown flag -%c (ignored)\n", *c);
				}
			}
		}
		else
		{
			pos[(*npos)++] = argv[i];
		}
	}
}

/*
 * vm_main - command-line entry point: dispatch to do_vm_dump or do_vm_diff
 * based on argv[1].
 */
int
vm_main(int argc, char **argv)
{
	VmOptions	options = {0};
	char	   *pos[8];
	int			npos = 0;

	if (argc < 2)
	{
		vm_usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (strcmp(argv[1], "dump") == 0)
	{
		parse_vm_flags(argc, argv, 2, &options, pos, &npos);
		if (npos != 2)
		{
			vm_usage(argv[0]);
			return EXIT_FAILURE;
		}
		return do_vm_dump(pos[0], pos[1], &options) == VM_SUCCESS
			? EXIT_SUCCESS
			: EXIT_FAILURE;
	}
	else if (strcmp(argv[1], "diff") == 0)
	{
		parse_vm_flags(argc, argv, 2, &options, pos, &npos);
		if (npos != 3)
		{
			vm_usage(argv[0]);
			return EXIT_FAILURE;
		}
		return do_vm_diff(pos[0], pos[1], pos[2], &options) == VM_SUCCESS
			? EXIT_SUCCESS
			: EXIT_FAILURE;
	}

	vm_usage(argv[0]);
	return EXIT_FAILURE;
}
