/*
 *
 *   -H                headers (incl. fp_next_slot)
 *   -i                internal array (dump only)
 *   -s                leaf slots (dump only)
 *   -a                all of the above
 *   -q                add summary
 *   --range A-B       only print pages in [A,B] (children still traversed)
 *   --page N          shorthand for --range N-N
 *   --extra           full per-slot/per-node listing instead of compressed
 *                     ranges - requires --range/--page
 *   --heap-page N     dump only: locate heap page N in the FSM tree
 *   --min N     dump -s only: filter leaf slot RANGES by avail_bytes
 *   --max N
 *   --only-changed    diff only
 *
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "storage/bufpage.h"
#include "storage/fsm_internals.h"

#include "pg_fsm.h"

#define FSM_CATEGORIES 256
#define FSM_CAT_STEP (BLCKSZ / FSM_CATEGORIES)

/*
 * Generic growable array ("vector") of T
 */
#define DECLARE_VEC(VecType, ElemType)                                         \
	typedef struct {                                                           \
		ElemType *items;                                                       \
		long count, cap;                                                       \
	} VecType

#define VEC_PUSH(vec, ElemType, ...)                                           \
	do {                                                                       \
		if ((vec)->count == (vec)->cap) {                                      \
			long _vec_new_cap = (vec)->cap ? (vec)->cap * 2 : 16;              \
			void *_vec_new_items =                                             \
				realloc((vec)->items, _vec_new_cap * sizeof(ElemType));        \
			if (!_vec_new_items) {                                             \
				fprintf(stderr, "out of memory (realloc failed)\n");           \
				exit(1);                                                       \
			}                                                                  \
			(vec)->items = _vec_new_items;                                     \
			(vec)->cap = _vec_new_cap;                                         \
		}                                                                      \
		(vec)->items[(vec)->count++] = (ElemType){__VA_ARGS__};                \
	} while (0)

/*
 * TreeInfo - a page's position in the FSM tree.
 */
typedef struct
{
	long		page;			/* this page's index in the fsm file */
	int			level;			/* 0=root, 1=internal, 2=leaf */
	long		parent_id;
	long		logpageno;		/* 0-based index among same-level siblings */
}			TreeInfo;

/*
 * Help understend what where wrong during parse header
 */
typedef enum
{
	HEADER_OK, HEADER_WRONG_PAGESIZE, HEADER_INVALID
}			HeaderStatus;

/*
 * FsmResult - status code returned by the do_fsm_dump()/do_fsm_diff()
 */
typedef enum
{
FSM_FAIL = 0, FSM_SUCCESS = 1} FsmResult;

/*
 * FsmPageInfo - everything about one FSM page.
 */
typedef struct
{
	TreeInfo	tree;			/* position within the FSM tree */

	int			valid;			/* passed fsm_header_status() checks */
	int			allzero;		/* uninitialized (all-zero) hole page */

	HeaderStatus header_status; /* result of header sanity checks */
	const char *invalid_reason; /* text reason if invalid */

	int			fp_next_slot;	/* FSM-specific: next slot to search from */
	uint8		nodes[NodesPerPage];	/* internal + leaf category bytes */

	PageHeaderData header;		/* raw copy of the standard page header */
}			FsmPageInfo;

DECLARE_VEC(LongVec, long);

/*
 * One run of in a row equal-valued bytes
 */
typedef struct
{
	long		start,
				end;
	uint8		value;			/* the shared value across [start, end] */
}			ByteCompressedRange;

DECLARE_VEC(ByteCompressedRangeVec, ByteCompressedRange);

/*
 * One run where old/new byte pairs stay equal; used by the diff command
 */
typedef struct
{
	long		start,
				end;			/* inclusive index range in the source arrays */
	uint8		old_value,
				new_value;		/* the shared (old,new) pair across the run */
}			ByteDiffCompressedRange;

DECLARE_VEC(ByteDiffCompressedRangeVec, ByteDiffCompressedRange);

/*
 * Info to print statistic
 */
typedef struct
{
	uint8		max_cat;		/* highest free-space category on the page */
	long		max_slot;		/* slot (heap page offset) holding max_cat */
	double		total_avail;	/* sum of avail_bytes across all slots */
}			LeafPageAgg;

/*
 * Running totals collected while walking the tree for "dump -q"
 */
typedef struct
{
	long		n_root,
				n_internal,
				n_leaf,
				n_zero,
				n_invalid;		/* page counts by kind/status */

	uint8		global_max_cat; /* highest free-space category seen */
	long		global_max_cat_page,
				global_max_cat_slot;	/* where global_max_cat was find */

	double		total_avail;	/* sum of avail_bytes over all leaf slots */
	long		total_slots;	/* number of leaf slots seen */
	long		cat_count[FSM_CATEGORIES];	/* histogram of categories */
}			DumpStats;

/*
 * Running totals collected while comparing two files for "diff -q"
 */
typedef struct
{
	long		pages_changed,
				headers_changed,
				headers_structural_changed,
				slots_changed,
				slots_up,
				slots_down,
				pages_added,
				pages_removed;	/* counts by kind of change */

	double		old_total_avail,
				new_total_avail;	/* avail_bytes before/after */
}			DiffStats;

/*
 * One heap-page range whose avail_bytes changed between old and new
 */
typedef struct
{
	long		heap_start,
				heap_end;		/* heap page range */

	long		delta_bytes_per_page;	/* new avail_bytes - old avail_bytes */
}			LeafDelta;

DECLARE_VEC(LeafDeltaVec, LeafDelta);

/*
 * cat_to_bytes - convert an FSM free-space category (0-255) to an
 * approximate byte count.
 */
static unsigned
cat_to_bytes(uint8 cat)
{
	if (cat == 255)
	{
		return (unsigned) MaxHeapTupleSize;
	}
	return (unsigned) cat * FSM_CAT_STEP;
}

/*
 *fsm_page_is_all_zero - true if every byte of the page buffer is zero
 */
static int
fsm_page_is_all_zero(const uint8 *buf)
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
 * fsm_header_status - sanity-check a page header (size, version, and the
 * lower/upper/special offset ordering).
 */
static HeaderStatus fsm_header_status(PageHeader ph, const char **reason)
{
	uint16		pagesize = ph->pd_pagesize_version & 0xFF00;
	uint16		version = ph->pd_pagesize_version & 0x00FF;

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
	if (ph->pd_special > BLCKSZ)
	{
		*reason = "pd_special is larger than the page";
		return HEADER_INVALID;
	}
	if (ph->pd_lower > ph->pd_upper)
	{
		*reason = "pd_lower is greater than pd_upper";
		return HEADER_INVALID;
	}
	if (ph->pd_upper > ph->pd_special)
	{
		*reason = "pd_upper is greater than pd_special";
		return HEADER_INVALID;
	}
	*reason = NULL;
	return HEADER_OK;
}

/* ? */
/*
 * classify_fsm_page - compute a page's place in the FSM tree
 */
static void
classify_fsm_page(long page_index, int *level, long *parent_id,
				  long *logpageno)
{
	long		group_size;
	long		idx;
	long		group;
	long		offset;

	if (page_index == 0)
	{
		*level = 0;
		*parent_id = -1;
		*logpageno = 0;
		return;
	}

	group_size = LeafNodesPerPage + 1;
	idx = page_index - 1;
	group = idx / group_size;
	offset = idx % group_size;

	*level = (offset == 0) ? 1 : 2;
	*parent_id = (*level == 1) ? 0 : (1 + group * group_size);
	*logpageno =
		(*level == 1) ? group : (group * LeafNodesPerPage + offset - 1);
}

/*
 * heap_page_to_fsm_slot - inverse of classify_fsm_page for leaf pages.
 */
static void
heap_page_to_fsm_slot(long heap_page, long *out_fsm_page,
					  long *out_slot)
{
	long		leaf_logpageno;
	long		group_size;
	long		group;
	long		offset;

	leaf_logpageno = heap_page / LeafNodesPerPage;
	group_size = LeafNodesPerPage + 1;
	group = leaf_logpageno / LeafNodesPerPage;
	offset = (leaf_logpageno % LeafNodesPerPage) + 1;

	*out_fsm_page = group * group_size + offset + 1;
	*out_slot = heap_page % LeafNodesPerPage;
}

/*
 * fill_fsm_page_info - classify its tree position, check all-zero page,
 * validate its header ...
 */
static void
fill_fsm_page_info(const uint8 *buf, long page_index,
				   FsmPageInfo * page_info)
{
	PageHeader	page_header;
	const char *reason = NULL;
	FSMPage		fsm_page;
	TreeInfo   *tr;

	page_header = (PageHeader) buf;
	page_info->tree.page = page_index;
	tr = &(page_info->tree);

	classify_fsm_page(page_index, &tr->level, &tr->parent_id, &tr->logpageno);

	page_info->allzero = fsm_page_is_all_zero(buf);
	page_info->header_status = page_info->allzero
		? HEADER_OK
		: fsm_header_status(page_header, &reason);
	page_info->invalid_reason = reason;
	page_info->valid =
		!page_info->allzero && page_info->header_status == HEADER_OK;
	page_info->header = *page_header;

	fsm_page = (FSMPage) PageGetContents((Page) buf);

	page_info->fp_next_slot = fsm_page->fp_next_slot;

	if (page_info->valid)
	{
		memcpy(page_info->nodes, fsm_page->fp_nodes, NodesPerPage);
	}
	else
	{
		memset(page_info->nodes, 0, NodesPerPage);
	}
}

/*
 * load_fsm - read FSM file into memory page by page. Returns an array of
 * FsmPageInfo.
 */
static FsmPageInfo * load_fsm(const char *path, long *out_total_pages)
{
	FILE	   *f;
	long		filesize;
	long		total_pages;
	FsmPageInfo *pages;
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
		fprintf(stderr, "%s: empty or truncated\n", path);
		fclose(f);
		return NULL;
	}

	pages = calloc(total_pages, sizeof(FsmPageInfo));
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
			fprintf(stderr, "%s: short read at page %ld,take as zero\n", path,
					page_index);
			memset(buf, 0, BLCKSZ);
		}
		fill_fsm_page_info(buf, page_index, &pages[page_index]);
	}
	fclose(f);
	*out_total_pages = total_pages;
	return pages;
}

/*
 * build_fsm_children - build a parent -> children index over the whole
 * FSM tree.
 */
static LongVec * build_fsm_children(long total_pages)
{
	LongVec    *by_parent;
	long		page_index;

	by_parent = calloc(total_pages, sizeof(LongVec));
	if (!by_parent)
	{
		fprintf(stderr, "out of memory building children index (%ld page(s))\n",
				total_pages);
		return NULL;
	}

	for (page_index = 0; page_index < total_pages; page_index++)
	{
		int			level;
		long		parent_id,
					logpageno;

		classify_fsm_page(page_index, &level, &parent_id, &logpageno);
		if (parent_id >= 0 && parent_id < total_pages)
		{
			VEC_PUSH(&by_parent[parent_id], long, page_index);
		}
	}
	return by_parent;
}

static void
free_fsm_children(LongVec * by_parent, long total_pages)
{
	long		page_index;

	if (!by_parent)
	{
		return;
	}

	for (page_index = 0; page_index < total_pages; page_index++)
	{
		free(by_parent[page_index].items);
	}
	free(by_parent);
}

/*
 * compress_byte_range - run-length compress a byte array into
 * [start,end]=value ranges.
 */
static ByteCompressedRangeVec compress_byte_range(const uint8 *arr, long n)
{
	ByteCompressedRangeVec compressed = {0};
	long		start;
	uint8		val;
	long		i;

	if (n <= 0)
	{
		return compressed;
	}

	start = 0;
	val = arr[0];

	for (i = 1; i < n; i++)
	{
		if (arr[i] != val)
		{
			VEC_PUSH(&compressed, ByteCompressedRange, start, i - 1, val);
			start = i;
			val = arr[i];
		}
	}
	VEC_PUSH(&compressed, ByteCompressedRange, start, n - 1, val);
	return compressed;
}

/*
 * compress_byte_diff_range - like compress_byte_range,
 * but over a pair of arrays
 */
static ByteDiffCompressedRangeVec
compress_byte_diff_range(const uint8 *a, const uint8 *b, long n)
{
	ByteDiffCompressedRangeVec compressed = {0};
	long		start;
	uint8		ov,
				nv;
	long		i;

	if (n <= 0)
	{
		return compressed;
	}

	start = 0;
	ov = a[0];
	nv = b[0];

	for (i = 1; i < n; i++)
	{
		if (a[i] != ov || b[i] != nv)
		{
			VEC_PUSH(&compressed, ByteDiffCompressedRange, start, i - 1, ov,
					 nv);
			start = i;
			ov = a[i];
			nv = b[i];
		}
	}
	VEC_PUSH(&compressed, ByteDiffCompressedRange, start, n - 1, ov, nv);
	return compressed;
}

/*
 * compute_leaf_agg - summarize one leaf page's slots: the highest
 * free-space category, and the total available bytes.
 */
static LeafPageAgg compute_leaf_agg(const uint8 *leaf_nodes, long n)
{
	LeafPageAgg agg = {0, -1, 0.0};
	long		i;

	for (i = 0; i < n; i++)
	{
		uint8		c = leaf_nodes[i];

		if (c > agg.max_cat)
		{
			agg.max_cat = c;
			agg.max_slot = i;
		}
		agg.total_avail += cat_to_bytes(c);
	}
	return agg;
}

/*
 * in_page_range - true if page in --range/--page filter
 */
static int
in_page_range(const FsmOptions * options, long id)
{
	if (!options->has_range)
	{
		return 1;
	}
	return id >= options->range_lo && id <= options->range_hi;
}

/*
 * in_avail_range - true if avail_bytes in --min/--max filter
 */
static int
in_avail_range(const FsmOptions * options, unsigned avail_bytes)
{
	if (options->has_min_avail && avail_bytes < (unsigned) options->min_avail)
	{
		return 0;
	}
	if (options->has_max_avail && avail_bytes > (unsigned) options->max_avail)
	{
		return 0;
	}
	return 1;
}

/*
 * print_header_line - print one page's header fields.
 */
static void
print_header_line(FILE *out, const char *indent,
				  const FsmPageInfo * page_info)
{
	PageHeaderData header;
	unsigned long long lsn = 0;

	header = page_info->header;

	fprintf(out, "%s  header:", indent);

#if PG_VERSION_NUM >= 190000
		lsn = (unsigned long long) PageXLogRecPtrGet(&header.pd_lsn);
#elif PG_VERSION_NUM >= 140000
		lsn = (unsigned long long) PageXLogRecPtrGet(header.pd_lsn);
#else
		lsn = ((unsigned long long) header.pd_lsn.xlogid << 32) | header.pd_lsn.xrecoff;
#endif

	fprintf(out, " pd_lsn=%llX", lsn);

	fprintf(out, " pd_checksum=%u", header.pd_checksum);
	fprintf(out, " pd_flags=0x%x", header.pd_flags);

	fprintf(out, " pd_lower=%u", header.pd_lower);
	fprintf(out, " pd_upper=%u", header.pd_upper);
	fprintf(out, " pd_special=%u", header.pd_special);

	fprintf(out, " pagesize=%u",
			(unsigned) (header.pd_pagesize_version & 0xFF00));
	fprintf(out, " layout_version=%u",
			(unsigned) (header.pd_pagesize_version & 0x00FF));
	fprintf(out, " fp_next_slot=%d\n", page_info->fp_next_slot);

	if (page_info->header_status == HEADER_INVALID)
	{
		fprintf(out, "%s  header valid: no (%s)\n", indent,
				page_info->invalid_reason);
	}
}

/*
 * print_internal_compressed - print a page's internal (non-leaf + leaf)
 * category bytes as run-length compressed ranges, split into the NONLEAF
 * and LEAF sections.
 */
static void
print_internal_compressed(FILE *out, const char *indent,
						  const FsmPageInfo * page_info)
{
	struct
	{
		const char *section_name;
		long		start,
					len;
	}			sec[2] = {
		{"NONLEAF", 0, NonLeafNodesPerPage},
		{"LEAF", NonLeafNodesPerPage, LeafNodesPerPage},
	};
	int			s;

	for (s = 0; s < 2; s++)
	{
		ByteCompressedRangeVec compressed;
		long		i;

		compressed =
			compress_byte_range(page_info->nodes + sec[s].start, sec[s].len);

		fprintf(out, "%s  %s (%ld nodes):\n", indent, sec[s].section_name,
				sec[s].len);

		for (i = 0; i < compressed.count; i++)
		{
			const		ByteCompressedRange *r = &compressed.items[i];
			char		range_str[32];

			if (r->start == r->end)
			{
				snprintf(range_str, sizeof(range_str), "[%ld]", r->start);
			}
			else
			{
				snprintf(range_str, sizeof(range_str), "[%ld-%ld]", r->start,
						 r->end);
			}

			fprintf(out, "%s      %-20s %3u\n", indent, range_str, r->value);
		}

		free(compressed.items);
	}
}

/*
 * print_internal_expanded - print a page's internal category bytes fully
 * expanded instead of run-length compressed, for the --extra flag.
 */
static void
print_internal_expanded(FILE *out, const char *indent,
						const FsmPageInfo * page_info)
{
	struct
	{
		const char *section_name;
		long		start,
					len;
	}			sec[2] = {
		{"NONLEAF", 0, NonLeafNodesPerPage},
		{"LEAF", NonLeafNodesPerPage, LeafNodesPerPage},
	};
	int			s;

	for (s = 0; s < 2; s++)
	{
		long		end;
		long		i;

		fprintf(out, "%s  %s (%ld node(s), expanded):\n", indent,
				sec[s].section_name, sec[s].len);
		end = sec[s].start + sec[s].len;

		for (i = sec[s].start; i < end; i++)
		{
			long		local_i = i - sec[s].start;

			if (local_i % 32 == 0)
			{
				fprintf(out, "%s      [%4ld] ", indent, local_i);
			}
			fprintf(out, "%3u ", page_info->nodes[i]);
			if (local_i % 32 == 31)
			{
				fprintf(out, "\n");
			}
		}
		if (sec[s].len % 32 != 0)
		{
			fprintf(out, "\n");
		}
	}
}

/*
 * print_leaf_compressed - print a leaf page's per-slot free-space
 * categories as run-length compressed heap-page ranges.
 */
static void
print_leaf_compressed(FILE *out, const char *indent,
					  const FsmPageInfo * page_info,
					  const FsmOptions * options)
{
	const uint8 *leaf = page_info->nodes + NonLeafNodesPerPage;
	ByteCompressedRangeVec compressed;
	long		base;
	long		i;

	base = page_info->tree.logpageno * LeafNodesPerPage;
	compressed = compress_byte_range(leaf, LeafNodesPerPage);

	fprintf(out, "%s  leaf slots (%d, starting at %ld)", indent,
			(int) LeafNodesPerPage, base);

	if (options->has_min_avail || options->has_max_avail)
	{
		fprintf(out, " [filtered by avail_bytes]");
	}
	fprintf(out, ":\n");

	for (i = 0; i < compressed.count; i++)
	{
		const		ByteCompressedRange *r = &compressed.items[i];
		unsigned	avail = cat_to_bytes(r->value);
		long		hp_start = base + r->start;
		long		hp_end = base + r->end;
		char		range_label[32];

		if (!in_avail_range(options, avail))
		{
			continue;
		}

		if (hp_start == hp_end)
		{
			snprintf(range_label, sizeof(range_label), "%8ld", hp_start);
		}
		else
		{
			snprintf(range_label, sizeof(range_label), "%8ld-%-8ld", hp_start,
					 hp_end);
		}

		fprintf(out, "%s    heap page %-17s: category=%-3u avail_bytes=%-5u",
				indent, range_label, r->value, avail);

		if (hp_start != hp_end)
		{
			fprintf(out, " (%ld page(s))", hp_end - hp_start + 1);
		}
		fprintf(out, "\n");
	}

	free(compressed.items);
}

/*
 * print_leaf_expanded - print a leaf page's per-slot free-space
 * categories one slot at a time (no run-length compression)
 */
static void
print_leaf_expanded(FILE *out, const char *indent,
					const FsmPageInfo * page_info,
					const FsmOptions * options)
{
	const uint8 *leaf = page_info->nodes + NonLeafNodesPerPage;
	long		s;

	fprintf(out,
			"%s  leaf slots (%d, one per heap page starting at %ld, expanded)",
			indent, (int) LeafNodesPerPage,
			page_info->tree.logpageno * LeafNodesPerPage);
	if (options->has_min_avail || options->has_max_avail)
	{
		fprintf(out, " [filtered by avail_bytes]");
	}
	fprintf(out, ":\n");
	for (s = 0; s < LeafNodesPerPage; s++)
	{
		unsigned	avail = cat_to_bytes(leaf[s]);

		if (!in_avail_range(options, avail))
		{
			continue;
		}
		fprintf(
				out,
				"%s    slot %4ld (heap page %8ld): category=%3u avail_bytes=%5u\n",
				indent, s, page_info->tree.logpageno * LeafNodesPerPage + s,
				leaf[s], avail);
	}
}

/*
 * fsm_kind_name - human-readable name for a tree level
 */
static const char *
fsm_kind_name(int level)
{
	if (level == 0)
	{
		return "ROOT";
	}
	if (level == 1)
	{
		return "INTERNAL";
	}
	return "LEAF";
}

/*
 * dump_zero_page - print an all-zero (uninitialized) page
 */
static void
dump_zero_page(FILE *out, const char *prefix, const char *branch,
			   long id, const char *kind_name,
			   const char *child_indent,
			   const FsmPageInfo * page_info, int show)
{
	if (!show)
	{
		return;
	}
	fprintf(out, "%s%s%ld fsm [%s] -- empty (uninitialized, all-zero page)\n",
			prefix, branch, id, kind_name);

	if (page_info->tree.level == 2)
	{
		long		hp_start = page_info->tree.logpageno * LeafNodesPerPage;
		long		hp_end = hp_start + LeafNodesPerPage - 1;

		fprintf(out, "%s  covers heap pages %ld-%ld\n\n", child_indent,
				hp_start, hp_end);
	}
}

/*
 * dump_wrong_pagesize_page - print a page whose header
 * pagesize mismatches BLCKSZ
 */
static void
dump_wrong_pagesize_page(FILE *out, const char *prefix,
						 const char *branch, long id,
						 const char *kind_name,
						 const FsmPageInfo * page_info,
						 const FsmOptions * options, int show)
{
	if (!show)
	{
		return;
	}
	fprintf(out,
			"%s%s%ld fsm [%s] -- WRONG PAGE SIZE, rebuild pg_fsm with the "
			"server's --with-blocksize\n",
			prefix, branch, id, kind_name);
	if (options->show_headers)
	{
		print_header_line(out, prefix, page_info);
	}
}

/*
 * dump_invalid_header_page - print a page that failed
 * header sanity checks
 */
static void
dump_invalid_header_page(FILE *out, const char *prefix,
						 const char *branch, long id,
						 const char *kind_name,
						 const FsmPageInfo * page_info,
						 const FsmOptions * options, int show)
{
	if (!show)
	{
		return;
	}
	fprintf(out, "%s%s%ld fsm [%s] -- INVALID HEADER (%s)\n", prefix, branch,
			id, kind_name, page_info->invalid_reason);
	if (options->show_headers)
	{
		print_header_line(out, prefix, page_info);
	}
}

/*
 * dump_valid_page_summary - print header/internal-array
 * part of a valid page
 */
static void
dump_valid_page_summary(FILE *out, const char *prefix,
						const char *branch, long id,
						const char *kind_name,
						const char *child_indent,
						const FsmPageInfo * page_info,
						const FsmOptions * options, int show)
{
	if (!show)
	{
		return;
	}
	fprintf(out, "%s%s%ld fsm [%s]\n", prefix, branch, id, kind_name);

	if (options->show_headers)
	{
		print_header_line(out, child_indent, page_info);
	}
	if (options->show_internal)
	{
		if (options->expand)
		{
			print_internal_expanded(out, child_indent, page_info);
		}
		else
		{
			print_internal_compressed(out, child_indent, page_info);
		}
	}
}

/*
 * update_leaf_stats
 */
static LeafPageAgg update_leaf_stats(const FsmPageInfo * page_info, long id,
									 DumpStats * stats)
{
	const uint8 *leaf = page_info->nodes + NonLeafNodesPerPage;
	LeafPageAgg agg = compute_leaf_agg(leaf, LeafNodesPerPage);
	long		slot;

	stats->total_avail += agg.total_avail;
	stats->total_slots += LeafNodesPerPage;
	for (slot = 0; slot < LeafNodesPerPage; slot++)
	{
		stats->cat_count[leaf[slot]]++;
	}
	if (agg.max_cat > stats->global_max_cat)
	{
		stats->global_max_cat = agg.max_cat;
		stats->global_max_cat_page = id;
		stats->global_max_cat_slot = agg.max_slot;
	}
	return agg;
}

/*
 * dump_leaf_details - print per-leaf-page aggregate + slot listing
 */
static void
dump_leaf_details(FILE *out, const char *child_indent,
				  const FsmPageInfo * page_info,
				  const FsmOptions * options, const LeafPageAgg * agg,
				  int show)
{
	if (!show)
	{
		return;
	}
	fprintf(out, "%s  page max: category=%u (%u bytes) at slot %ld\n",
			child_indent, agg->max_cat, cat_to_bytes(agg->max_cat),
			agg->max_slot);
	fprintf(out, "%s  Free space on page: %.0f \n\n", child_indent,
			agg->total_avail);

	if (options->show_slots)
	{
		if (options->expand)
		{
			print_leaf_expanded(out, child_indent, page_info, options);
		}
		else
		{
			print_leaf_compressed(out, child_indent, page_info, options);
		}
	}
}

/*
 * dump_node - recursively print one FSM page and its subtree.
 */
static void
dump_node(FILE *out, FsmPageInfo * pages, LongVec * children,
		  long total_pages, long id, const char *prefix,
		  int is_last, const FsmOptions * options,
		  DumpStats * stats)
{
	FsmPageInfo *page_info = &pages[id];
	const char *branch = "\\-- ";
	const char *kind_name = fsm_kind_name(page_info->tree.level);
	int			show = in_page_range(options, id);
	char		child_indent[512];
	LongVec    *kids;
	long		i;

	snprintf(child_indent, sizeof(child_indent), "%s    ", prefix);

	if (page_info->tree.level == 0)
	{
		stats->n_root++;
	}
	else if (page_info->tree.level == 1)
	{
		stats->n_internal++;
	}
	else
	{
		stats->n_leaf++;
	}

	if (page_info->allzero)
	{
		stats->n_zero++;
		dump_zero_page(out, prefix, branch, id, kind_name, child_indent,
					   page_info, show);
	}
	else if (page_info->header_status == HEADER_WRONG_PAGESIZE)
	{
		stats->n_invalid++;
		dump_wrong_pagesize_page(out, prefix, branch, id, kind_name, page_info,
								 options, show);
	}
	else if (!page_info->valid)
	{
		stats->n_invalid++;
		dump_invalid_header_page(out, prefix, branch, id, kind_name, page_info,
								 options, show);
	}
	else
	{
		dump_valid_page_summary(out, prefix, branch, id, kind_name,
								child_indent, page_info, options, show);

		if (page_info->tree.level == 2)
		{
			LeafPageAgg agg = update_leaf_stats(page_info, id, stats);

			dump_leaf_details(out, child_indent, page_info, options, &agg,
							  show);
		}
	}

	if (id >= total_pages)
	{
		return;
	}
	kids = &children[id];

	for (i = 0; i < kids->count; i++)
	{
		dump_node(out, pages, children, total_pages, kids->items[i],
				  child_indent, i == kids->count - 1, options, stats);
	}
}

/*
 * report_heap_page_lookup - locate which FSM page and
 * slot a given heap page maps to.
 */
static void
report_heap_page_lookup(FILE *out, FsmPageInfo * pages,
						long total_pages,
						const FsmOptions * options)
{
	long		fsm_page,
				slot;
	FsmPageInfo *page_info;
	uint8		cat;

	heap_page_to_fsm_slot(options->heap_page_query, &fsm_page, &slot);

	fprintf(out, "heap page %ld -> fsm page %ld, slot %ld",
			options->heap_page_query, fsm_page, slot);

	if (fsm_page >= total_pages)
	{
		fprintf(out, " - file only has %ld page(s), not covered yet\n",
				total_pages);
		return;
	}
	page_info = &pages[fsm_page];

	if (page_info->allzero)
	{
		fprintf(out, " - page is empty (uninitialized, all-zero)\n");
		return;
	}
	if (!page_info->valid)
	{
		fprintf(out, " - page header is bad (%s)\n", page_info->invalid_reason);
		return;
	}
	cat = page_info->nodes[NonLeafNodesPerPage + slot];

	fprintf(out, ": category=%u avail_bytes=%u\n", cat, cat_to_bytes(cat));
	if (options->show_headers)
	{
		print_header_line(out, "", page_info);
	}
}

/*
 * print info how many pages  belongs to category
 */
static void
print_page_by_category(FILE *out, const DumpStats * stats)
{
	fprintf(out, "\nheap pages by category:\n");
	{
		int			cat;

		for (cat = 0; cat < FSM_CATEGORIES; cat++)
		{
			if (stats->cat_count[cat] == 0)
			{
				continue;
			}
			fprintf(out, "  category %3d (%5u bytes): %ld page(s)\n", cat,
					cat_to_bytes((uint8) cat), stats->cat_count[cat]);
		}
	}
}

/*
 * print entire information about flags before printing tree
 */
static void
print_entire_info(FILE *out, const FsmOptions * options,
				  const char *in_path, long total_pages)
{
	fprintf(out, "=== pg_fsm dump: %s ===\n", in_path);
	fprintf(out, "file size: %ld bytes, total pages: %ld (%d bytes/page)\n",
			total_pages * BLCKSZ, total_pages, BLCKSZ);

	fprintf(out, "flags: headers=%s internal=%s slots=%s expand=%s\n",
			options->show_headers ? "on" : "off",
			options->show_internal ? "on" : "off",
			options->show_slots ? "on" : "off", options->expand ? "on" : "off");
	if (options->has_range)
	{
		fprintf(out,
				"printing only pages [%ld, %ld] (SUMMARY covers the "
				"whole file)\n",
				options->range_lo, options->range_hi);
	}
	return;
}

/*
 * print_summary - 	print summary information (-q) built
 * from the DumpStats it accumulated.
 */
static void
print_summary(FILE *out, const DumpStats * stats, long total_pages)
{
	fprintf(out, "\n-------------------------------------------------------"
			"-------\n");
	fprintf(out, "SUMMARY\n");
	fprintf(out,
			"--------------------------------------------------------------\n");
	fprintf(out, "root pages: %ld  internal pages: %ld  leaf pages: %ld\n",
			stats->n_root, stats->n_internal, stats->n_leaf);
	fprintf(out, "zero/hole pages: %ld  invalid-header pages: %ld\n",
			stats->n_zero, stats->n_invalid);
	fprintf(out,
			"\nMax free-space category: %u (%u bytes), page %ld slot %ld\n",
			stats->global_max_cat, cat_to_bytes(stats->global_max_cat),
			stats->global_max_cat_page, stats->global_max_cat_slot);
	fprintf(out, "\nTotal avail_bytes : %.0f\n", stats->total_avail);

	print_page_by_category(out, stats);
}

/*
 * do_fsm_dump - load the FSM file, then either report a single lookup, or
 * build the tree index and recursively print the whole tree via dump_node.
 */
static FsmResult do_fsm_dump(const char *in_path, const char *out_path,
							 const FsmOptions * options)
{
	long		total_pages;
	FsmPageInfo *pages;
	FILE	   *out;
	LongVec    *children;
	DumpStats	stats = {0};

	pages = load_fsm(in_path, &total_pages);
	if (!pages)
	{
		return FSM_FAIL;
	}

	out = fopen(out_path, "w");
	if (!out)
	{
		perror(out_path);
		free(pages);
		return FSM_FAIL;
	}

	if (options->has_heap_page)
	{
		fprintf(out, "=== pg_fsm dump: %s (--heap-page %ld lookup) ===\n",
				in_path, options->heap_page_query);
		report_heap_page_lookup(out, pages, total_pages, options);
		fclose(out);
		free(pages);
		fprintf(stderr, "wrote %s\n", out_path);
		return FSM_SUCCESS;
	}

	children = build_fsm_children(total_pages);
	if (!children)
	{
		fclose(out);
		free(pages);
		return FSM_FAIL;
	}

	print_entire_info(out, options, in_path, total_pages);
	fprintf(out, "\n");

	dump_node(out, pages, children, total_pages, 0, "", 1, options, &stats);

	if (options->stats)
	{
		print_summary(out, &stats, total_pages);
	}

	fclose(out);
	free_fsm_children(children, total_pages);
	free(pages);
	fprintf(stderr, "wrote %s\n", out_path);
	return FSM_SUCCESS;
}

/* Status - how a page compares between the old and new file in "diff". */
typedef enum
{
	ST_SAME, ST_CHANGED, ST_ADDED, ST_REMOVED, ST_EMPTY
}			Status;

static int	header_structural_changed(const PageHeaderData *old_hdr,
									  const PageHeaderData *new_hdr,
									  int old_next_slot, int new_next_slot);
static int	header_changed(const PageHeaderData *old_hdr,
						   const PageHeaderData *new_hdr, int structural);

/*
 * page_status - classify a page's change status between two files by
 * comparing
 */
static Status page_status(const FsmPageInfo * old_page,
						  const FsmPageInfo * new_page)
{
	int			old_ok = old_page && !old_page->allzero && old_page->valid;
	int			new_ok = new_page && !new_page->allzero && new_page->valid;
	int			nodes_changed;
	int			structural;
	int			hdr_changed;

	if (!old_ok && new_ok)
	{
		return ST_ADDED;
	}
	if (old_ok && !new_ok)
	{
		return ST_REMOVED;
	}
	if (!old_ok && !new_ok)
	{
		return ST_EMPTY;
	}

	nodes_changed = memcmp(old_page->nodes, new_page->nodes, NodesPerPage) != 0;

	structural = header_structural_changed(&old_page->header, &new_page->header,
										   old_page->fp_next_slot,
										   new_page->fp_next_slot);
	hdr_changed = header_changed(&old_page->header, &new_page->header, structural);

	if (nodes_changed || hdr_changed)
	{
		return ST_CHANGED;
	}
	return ST_SAME;
}

/* status_name - human-readable label for a Status value. */
static const char *
status_name(Status s)
{
	switch (s)
	{
		case ST_SAME:
			return "same";
		case ST_CHANGED:
			return "CHANGED";
		case ST_ADDED:
			return "ADDED";
		case ST_REMOVED:
			return "REMOVED";
		default:
			return "empty";
	}
}

/*
 * print_internal_diff - print the changed internal-node ranges between
 * two pages, run-length compressed via compress_byte_diff_range.
 */
static void
print_internal_diff(FILE *out, int do_print, const char *indent,
					const FsmPageInfo * old_page,
					const FsmPageInfo * new_page,
					const FsmOptions * options, DiffStats * stats)
{
	ByteDiffCompressedRangeVec compressed;
	long		i;

	compressed = compress_byte_diff_range(old_page->nodes, new_page->nodes,
										  NonLeafNodesPerPage);

	for (i = 0; i < compressed.count; i++)
	{
		const		ByteDiffCompressedRange *r = &compressed.items[i];
		int			changed = r->old_value != r->new_value;
		const char *tag = changed ? "CHANGED" : "same";

		if (changed)
		{
			stats->slots_changed += r->end - r->start + 1;
			if (r->new_value > r->old_value)
			{
				stats->slots_up += r->end - r->start + 1;
			}
			else
			{
				stats->slots_down += r->end - r->start + 1;
			}
		}

		/*
		 * Without --only-changed, every range is printed (tagged [same] or
		 * [CHANGED])
		 */
		if (options->only_changed && !changed)
		{
			continue;
		}
		if (!do_print)
		{
			continue;
		}
		if (options->expand)
		{
			long		j;

			for (j = r->start; j <= r->end; j++)
			{
				fprintf(out,
						"%s  internal-node %4ld           : %3u -> %3u  [%s]\n",
						indent, j, r->old_value, r->new_value, tag);
			}
		}
		else if (r->start == r->end)
		{
			fprintf(out, "%s  internal-node %4ld         : %3u -> %3u  [%s]\n",
					indent, r->start, r->old_value, r->new_value, tag);
		}
		else
		{
			fprintf(
					out,
					"%s  internal-node %4ld-%-4ld    : %3u -> %3u  [%s] (%ld node(s))\n",
					indent, r->start, r->end, r->old_value, r->new_value, tag,
					r->end - r->start + 1);
		}
	}
	free(compressed.items);
}

/*
 * print_leaf_diff - print the changed leaf-slot ranges between
 * two pages' avail_bytes. Every changed
 * range is recorded into deltas (for the file-wide delta report).
 */
static void
print_leaf_diff(FILE *out, int do_print, const char *indent,
				const FsmPageInfo * old_page,
				const FsmPageInfo * new_page,
				const FsmOptions * options, DiffStats * stats,
				LeafDeltaVec * deltas)
{
	const uint8 *old_leaf = old_page->nodes + NonLeafNodesPerPage;
	const uint8 *new_leaf = new_page->nodes + NonLeafNodesPerPage;
	ByteDiffCompressedRangeVec compressed;
	long		i;

	compressed = compress_byte_diff_range(old_leaf, new_leaf, LeafNodesPerPage);

	for (i = 0; i < compressed.count; i++)
	{
		const		ByteDiffCompressedRange *r = &compressed.items[i];
		long		n = r->end - r->start + 1;
		long		hp_start = old_page->tree.logpageno * LeafNodesPerPage + r->start;
		long		hp_end = old_page->tree.logpageno * LeafNodesPerPage + r->end;
		int			changed = r->old_value != r->new_value;
		const char *tag = changed ? "CHANGED" : "same";
		unsigned	ob,
					nb;

		ob = cat_to_bytes(r->old_value);
		nb = cat_to_bytes(r->new_value);

		if (changed)
		{
			stats->slots_changed += n;
			if (nb > ob)
			{
				stats->slots_up += n;
			}
			else
			{
				stats->slots_down += n;
			}
			VEC_PUSH(deltas, LeafDelta, hp_start, hp_end, (long) nb - (long) ob);
		}

		/*
		 * Without --only-changed, every range is printed (tagged [same] or
		 * [CHANGED])
		 */
		if (options->only_changed && !changed)
		{
			continue;
		}
		if (!do_print)
		{
			continue;
		}
		if (options->expand)
		{
			long		j;

			for (j = hp_start; j <= hp_end; j++)
			{
				fprintf(out,
						"%s  leaf-slot heap page %8ld           : %3u -> %3u "
						"(%5u -> %5u B)  [%s]\n",
						indent, j, r->old_value, r->new_value, ob, nb, tag);
			}
		}
		else if (hp_start == hp_end)
		{
			fprintf(out,
					"%s  leaf-slot heap page %8ld         : %3u -> %3u (%5u "
					"-> %5u "
					"B)  [%s]\n",
					indent, hp_start, r->old_value, r->new_value, ob, nb, tag);
		}
		else
		{
			fprintf(out,
					"%s  leaf-slot heap page %8ld-%-8ld: %3u -> %3u (%5u -> "
					"%5u B)  [%s] (%ld page(s))\n",
					indent, hp_start, hp_end, r->old_value, r->new_value, ob,
					nb, tag, n);
		}
	}
	free(compressed.items);
}

/*
 * diff_ref_page - pick which page's tree info to use for kind_name display
 */
static FsmPageInfo * diff_ref_page(FsmPageInfo * old_page,
								   FsmPageInfo * new_page)
{
	if (new_page && !new_page->allzero && new_page->valid)
	{
		return new_page;
	}
	return old_page;
}

/*
 * header_structural_changed - true if any structural header field differs
 */
static int
header_structural_changed(const PageHeaderData *old_hdr,
						  const PageHeaderData *new_hdr,
						  int old_next_slot, int new_next_slot)
{
	return old_hdr->pd_lower != new_hdr->pd_lower ||
		old_hdr->pd_upper != new_hdr->pd_upper ||
		old_hdr->pd_special != new_hdr->pd_special ||
		old_hdr->pd_flags != new_hdr->pd_flags ||
		old_hdr->pd_pagesize_version != new_hdr->pd_pagesize_version ||
		old_next_slot != new_next_slot;
}

/*
 * header_changed - true if structural fields, lsn, or checksum differ
 */
static int
header_changed(const PageHeaderData *old_hdr,
			   const PageHeaderData *new_hdr, int structural)
{
	unsigned long long old_lsn = 0;
	unsigned long long new_lsn = 0;

#if PG_VERSION_NUM >= 190000
		old_lsn = (unsigned long long) PageXLogRecPtrGet(&old_hdr->pd_lsn);
		new_lsn = (unsigned long long) PageXLogRecPtrGet(&new_hdr->pd_lsn);
#elif PG_VERSION_NUM >= 140000
		old_lsn = (unsigned long long) PageXLogRecPtrGet(old_hdr->pd_lsn);
		new_lsn = (unsigned long long) PageXLogRecPtrGet(new_hdr->pd_lsn);
#else 
		old_lsn = ((unsigned long long) old_hdr->pd_lsn.xlogid << 32) | old_hdr->pd_lsn.xrecoff;
		new_lsn = ((unsigned long long) new_hdr->pd_lsn.xlogid << 32) | new_hdr->pd_lsn.xrecoff;
#endif

	return structural ||
		old_lsn != new_lsn ||
		old_hdr->pd_checksum != new_hdr->pd_checksum;
}

/* report_header_diff - update header-change stats and print a note if shown */
static void
report_header_diff(FILE *out, const char *child_indent, int show,
				   const FsmOptions * options,
				   const PageHeaderData *old_hdr,
				   const PageHeaderData *new_hdr, int old_next_slot,
				   int new_next_slot, DiffStats * stats)
{
	int			structural = header_structural_changed(old_hdr, new_hdr, old_next_slot,
													   new_next_slot);

	if (!header_changed(old_hdr, new_hdr, structural))
	{
		return;
	}

	stats->headers_changed++;
	if (structural)
	{
		stats->headers_structural_changed++;
		if (show)
		{
			fprintf(out,
					"%s  header changed (structural: lower/upper/special/"
					"flags/version/fp_next_slot)\n",
					child_indent);
		}
	}
	else if (show && options->show_headers)
	{
		fprintf(out, "%s  header changed (lsn/checksum only)\n", child_indent);
	}
}

/*
 * report_changed_page - handle ST_CHANGED: header diff + internal/leaf diffs
 */
static void
report_changed_page(FILE *out, const char *child_indent, int show,
					const FsmOptions * options,
					FsmPageInfo * old_page, FsmPageInfo * new_page,
					DiffStats * stats, LeafDeltaVec * deltas)
{
	stats->pages_changed++;

	report_header_diff(out, child_indent, show, options, &old_page->header,
					   &new_page->header, old_page->fp_next_slot,
					   new_page->fp_next_slot, stats);

	if (options->show_internal)
	{
		print_internal_diff(out, show, child_indent, old_page, new_page,
							options, stats);
	}

	print_leaf_diff(out, show, child_indent, old_page, new_page, options,
					stats, deltas);
}

/*
 * diff_print_status_line - print "N fsm [KIND] STATUS" if show is set
 */
static void
diff_print_status_line(FILE *out, const char *prefix, long id,
					   const char *kind_name, Status status,
					   int show)
{
	static const char *branch = "\\-- ";

	if (!show)
	{
		return;
	}
	fprintf(out, "%s%s%ld fsm [%s] %s\n", prefix, branch, id, kind_name,
			status_name(status));
}

/*
 * diff_apply_status - update per-page-status counters
 */
static void
diff_apply_status(FILE *out, const char *child_indent, int show,
				  const FsmOptions * options, Status status,
				  FsmPageInfo * old_page, FsmPageInfo * new_page,
				  DiffStats * stats, LeafDeltaVec * deltas)
{
	switch (status)
	{
		case ST_ADDED:
			stats->pages_added++;
			break;
		case ST_REMOVED:
			stats->pages_removed++;
			break;
		case ST_CHANGED:
			report_changed_page(out, child_indent, show, options, old_page,
								new_page, stats, deltas);
			break;
		default:
			break;
	}
}

/*
 * prototype for recursion resolution
 */
static void diff_node(FILE *out, FsmPageInfo * old_pages, long total_old,
					  FsmPageInfo * new_pages, long total_new, LongVec * children,
					  long id, const char *prefix, int is_last,
					  const FsmOptions * options, DiffStats * stats,
					  LeafDeltaVec * deltas);

/*
 * diff_recurse_children - recurse into id's children with the given indent
 */
static void
diff_recurse_children(FILE *out, FsmPageInfo * old_pages,
					  long total_old, FsmPageInfo * new_pages,
					  long total_new, LongVec * children, long id,
					  const char *child_indent,
					  const FsmOptions * options, DiffStats * stats,
					  LeafDeltaVec * deltas)
{
	LongVec    *kids;
	long		i;

	if (id >= (total_old > total_new ? total_old : total_new))
	{
		return;
	}
	kids = &children[id];

	for (i = 0; i < kids->count; i++)
	{
		diff_node(out, old_pages, total_old, new_pages, total_new, children,
				  kids->items[i], child_indent, i == kids->count - 1, options,
				  stats, deltas);
	}
}

/*
 * diff_node - recursively compare one page between old/new FSM files
 * and print the result, updating DiffStats/deltas.
 */
static void
diff_node(FILE *out, FsmPageInfo * old_pages, long total_old,
		  FsmPageInfo * new_pages, long total_new, LongVec * children,
		  long id, const char *prefix, int is_last,
		  const FsmOptions * options, DiffStats * stats,
		  LeafDeltaVec * deltas)
{
	FsmPageInfo *old_page;
	FsmPageInfo *new_page;
	FsmPageInfo *ref;
	Status		status;
	const char *kind_name;
	int			show;
	char		child_indent[512];

	old_page = id < total_old ? &old_pages[id] : NULL;
	new_page = id < total_new ? &new_pages[id] : NULL;
	status = page_status(old_page, new_page);
	ref = diff_ref_page(old_page, new_page);
	kind_name = ref ? fsm_kind_name(ref->tree.level) : "?";

	show =
		in_page_range(options, id) &&
		!(options->only_changed && (status == ST_SAME || status == ST_EMPTY));

	snprintf(child_indent, sizeof(child_indent), "%s    ", prefix);

	diff_print_status_line(out, prefix, id, kind_name, status, show);
	diff_apply_status(out, child_indent, show, options, status, old_page,
					  new_page, stats, deltas);
	diff_recurse_children(out, old_pages, total_old, new_pages, total_new,
						  children, id, child_indent, options, stats, deltas);
}

/*
 * diff_print_header - print the "=== pg_fsm diff ===" preamble
 */
static void
diff_print_header(FILE *out, const char *old_path, long total_old,
				  const char *new_path, long total_new,
				  const FsmOptions * options)
{
	fprintf(out,
			"=== pg_fsm diff ===\nold: %s (%ld pages)\nnew: %s (%ld pages)\n",
			old_path, total_old, new_path, total_new);

	if (options->only_changed)
	{
		fprintf(out,
				"(--only-changed: SAME/empty subtrees and unchanged runs are "
				"hidden)\n");
	}
	if (options->has_range)
	{
		fprintf(out, "printing only pages [%ld, %ld]\n", options->range_lo,
				options->range_hi);
	}
	fprintf(out, "\n");

	if (total_new > total_old)
	{
		fprintf(out, "file GREW by %ld page(s)\n\n", total_new - total_old);
	}
	else if (total_old > total_new)
	{
		fprintf(out, "file SHRANK by %ld page(s)\n\n", total_old - total_new);
	}
}

/*
 * sum_leaf_avail - total avail_bytes across all valid/zero leaf pages
 */
static double
sum_leaf_avail(const FsmPageInfo * pages, long total_pages)
{
	double		total = 0;
	long		page_index;

	for (page_index = 0; page_index < total_pages; page_index++)
	{
		const		FsmPageInfo *page_info = &pages[page_index];

		if (page_info->tree.level == 2 &&
			(page_info->allzero || page_info->valid))
		{
			const uint8 *leaf = page_info->nodes + NonLeafNodesPerPage;

			total += compute_leaf_agg(leaf, LeafNodesPerPage).total_avail;
		}
	}
	return total;
}

/*
 * diff_print_summary - print the "-q" SUMMARY block from DiffStats
 */
static void
diff_print_summary(FILE *out, const DiffStats * stats)
{
	fprintf(out, "\n----------------------------------------------------------"
			"----\n");
	fprintf(out, "SUMMARY\n");
	fprintf(out,
			"--------------------------------------------------------------\n");
	fprintf(out,
			"pages changed: %ld  added: %ld  removed: %ld  headers changed: "
			"%ld \n",
			stats->pages_changed, stats->pages_added, stats->pages_removed,
			stats->headers_changed);
	fprintf(out,
			"node/slots changed: %ld (gained space: %ld, lost space: %ld)\n",
			stats->slots_changed, stats->slots_up, stats->slots_down);
	fprintf(out, "total avail_bytes: old=%.0f new=%.0f (delta %+.0f)\n",
			stats->old_total_avail, stats->new_total_avail,
			stats->new_total_avail - stats->old_total_avail);
}

/*
 * do_fsm_diff - implement the "diff" command: load both FSM files, then
 * recursively compare them page-by-page via diff_node and print an optional
 * summary (-q).
 */
static FsmResult do_fsm_diff(const char *old_path, const char *new_path,
							 const char *out_path, const FsmOptions * options)
{
	long		total_old,
				total_new;
	long		total_max;
	FsmPageInfo *old_pages;
	FsmPageInfo *new_pages;
	LongVec    *children;
	FILE	   *out;
	DiffStats	stats = {0};
	LeafDeltaVec deltas = {0};

	old_pages = load_fsm(old_path, &total_old);
	if (!old_pages)
	{
		return FSM_FAIL;
	}
	new_pages = load_fsm(new_path, &total_new);
	if (!new_pages)
	{
		free(old_pages);
		return FSM_FAIL;
	}

	total_max = total_old > total_new ? total_old : total_new;
	children = build_fsm_children(total_max);
	if (!children)
	{
		free(old_pages);
		free(new_pages);
		return FSM_FAIL;
	}

	out = fopen(out_path, "w");
	if (!out)
	{
		perror(out_path);
		free(old_pages);
		free(new_pages);
		free_fsm_children(children, total_max);
		return FSM_FAIL;
	}

	diff_print_header(out, old_path, total_old, new_path, total_new, options);

	diff_node(out, old_pages, total_old, new_pages, total_new, children, 0, "",
			  1, options, &stats, &deltas);

	stats.old_total_avail = sum_leaf_avail(old_pages, total_old);
	stats.new_total_avail = sum_leaf_avail(new_pages, total_new);

	if (options->stats)
	{
		diff_print_summary(out, &stats);
	}

	fclose(out);
	free(deltas.items);
	free_fsm_children(children, total_max);
	free(old_pages);
	free(new_pages);
	fprintf(stderr, "wrote %s\n", out_path);
	return FSM_SUCCESS;
}

/*
 * fsm_usage - print command-line usage/help text to stderr.
 */
static void
fsm_usage(const char *prog)
{
	fprintf(stderr,
			"usage:\n"
			"  %s -F dump [flags] <relfilenode_fsm> <out.txt>\n"
			"  %s -F diff [flags] <old_fsm> <new_fsm> <out.txt>\n"
			"flags:\n"
			"  -H                headers (incl. fp_next_slot)  -i internal "
			"array (dump only)\n"
			"  -s                leaf slots (dump only)        -a all of "
			"the above\n"
			"  -q                add summary\n"
			"  --range A-B       only print pages in [A,B] (children still "
			"traversed)\n"
			"  --page N          shorthand for --range N-N\n"
			"  --extra          full per-slot/per-node listing instead of "
			"compressed ranges (requires --range/--page)\n"
			"  --heap-page N     dump only: locate heap page N in the FSM "
			"tree\n"
			"  --min N     dump -s only: filter leaf slot ranges by "
			"avail_bytes\n"
			"  --max N\n"
			"  --only-changed    diff only: hide SAME/empty subtrees and "
			"unchanged runs\n",
			prog, prog);
}

/*
 * parse_fsm_flags - parse command-line flags shared by "dump" and "diff"
 */
static void
parse_fsm_flags(int argc, char **argv, int start,
				FsmOptions * options, char **pos, int *npos)
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
				fprintf(stderr, "bad --range %s, want A-B\n", argv[i]);
			}
		}
		else if (strcmp(a, "--page") == 0 && i + 1 < argc)
		{
			options->has_range = 1;
			options->range_lo = options->range_hi = atol(argv[++i]);
		}
		else if (strcmp(a, "--extra") == 0)
		{
			options->expand = 1;
		}
		else if (strcmp(a, "--heap-page") == 0 && i + 1 < argc)
		{
			options->has_heap_page = 1;
			options->heap_page_query = atol(argv[++i]);
		}
		else if (strcmp(a, "--min") == 0 && i + 1 < argc)
		{
			options->has_min_avail = 1;
			options->min_avail = atol(argv[++i]);
		}
		else if (strcmp(a, "--max") == 0 && i + 1 < argc)
		{
			options->has_max_avail = 1;
			options->max_avail = atol(argv[++i]);
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
					case 'i':
						options->show_internal = 1;
						break;
					case 's':
						options->show_slots = 1;
						break;
					case 'a':
						options->show_headers = options->show_internal =
							options->show_slots = 1;
						break;
					case 'q':
						options->stats = 1;
						break;
					default:
						fprintf(stderr, "unknown flag -%c\n", *c);
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
 * fsm_main - command-line entry point: dispatch to do_fsm_dump or
 * do_fsm_diff based on argv[1].
 */
int
fsm_main(int argc, char **argv)
{
	FsmOptions	options = {0};
	char	   *pos[8];
	int			npos = 0;

	if (argc < 2)
	{
		fsm_usage(argv[0]);
		return EXIT_FAILURE;
	}

	options.stats = 0;

	if (strcmp(argv[1], "dump") == 0)
	{
		parse_fsm_flags(argc, argv, 2, &options, pos, &npos);
		if (npos != 2)
		{
			fsm_usage(argv[0]);
			return EXIT_FAILURE;
		}
		return do_fsm_dump(pos[0], pos[1], &options) == FSM_SUCCESS
			? EXIT_SUCCESS
			: EXIT_FAILURE;
	}
	else if (strcmp(argv[1], "diff") == 0)
	{
		parse_fsm_flags(argc, argv, 2, &options, pos, &npos);
		if (npos != 3)
		{
			fsm_usage(argv[0]);
			return EXIT_FAILURE;
		}
		return do_fsm_diff(pos[0], pos[1], pos[2], &options) == FSM_SUCCESS
			? EXIT_SUCCESS
			: EXIT_FAILURE;
	}

	fsm_usage(argv[0]);
	return EXIT_FAILURE;
}
