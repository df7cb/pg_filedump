#ifndef PG_FSM_H
#define PG_FSM_H

#include "postgres.h"

typedef struct
{
	int			show_headers;	/* -H: print page headers */
	int			show_internal;	/* -i: print internal nodes */
	int			show_slots;		/* -s: print leaf slots */

	int			stats;			/* -q: print summary */

	int			has_range;		/* --range/--page given */
	long		range_lo,
				range_hi;		/* page id bounds */

	int			expand;			/* --extra: no compression */

	int			has_heap_page;	/* --heap-page given */
	long		heap_page_query;	/* heap page to locate */
	int			has_min_avail;	/* --min given */
	long		min_avail;		/* avail_bytes lower bound */
	int			has_max_avail;	/* --max given */
	long		max_avail;		/* avail_bytes upper bound */
	int			only_changed;	/* --only-changed: diff only */
}			FsmOptions;

int			fsm_main(int argc, char **argv);

#endif
