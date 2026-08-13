#ifndef PG_VM_H
#define PG_VM_H

#include "postgres.h"

typedef struct
{
	int			show_headers;	/* -H: print page headers */
	int			stats;			/* -q: print summary */

	int			has_range;		/* --heap-range given */
	long		range_lo,
				range_hi;		/* heap page bounds */

	int			has_heap_page;	/* --heap-page given */
	long		heap_page_query;	/* heap page to look up */
	int			expand;			/* --expand: no compression */

	int			only_not_visible;	/* dump: hide visible pages */
	int			only_not_frozen;	/* dump: hide frozen pages */
	int			only_changed;	/* --only-changed: diff only */
}			VmOptions;

int			vm_main(int argc, char **argv);

#endif
