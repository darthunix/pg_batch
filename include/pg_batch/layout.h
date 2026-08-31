/* Logical column layout shared by independently built batch plan nodes. */
#ifndef PG_BATCH_LAYOUT_H
#define PG_BATCH_LAYOUT_H

#include "postgres.h"

#include "abi.h"

/*
 * A layout numbers the columns stored in a batch independently of a table.
 * target_columns maps Plan target-list positions to batch columns. A NULL
 * mapping means identity; otherwise -1 means that a target has no batch
 * representation.
 */
typedef struct PgBatchLayout
{
	/* Set by the caller to sizeof(PgBatchLayout). */
	Size		struct_size;
	/* Number of logical columns available from the batch producer. */
	int			ncolumns;
	/* Number of entries in the plan target list described below. */
	int			ntargets;
	/* Optional ntargets-entry map; NULL means target N uses column N. */
	const int  *target_columns;
} PgBatchLayout;

#define PG_BATCH_LAYOUT_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchLayout, target_columns)

static inline void
pg_batch_check_layout(const PgBatchLayout *layout)
{
	if (layout == NULL || layout->struct_size < PG_BATCH_LAYOUT_MIN_SIZE ||
		layout->ncolumns < 0 || layout->ntargets < 0 ||
		(layout->target_columns == NULL &&
		 layout->ntargets > layout->ncolumns))
		elog(ERROR, "pg_batch received an invalid logical layout");
	for (int target = 0; target < layout->ntargets; target++)
	{
		int			column = layout->target_columns == NULL ? target :
			layout->target_columns[target];

		if (column < -1 || column >= layout->ncolumns)
			elog(ERROR, "pg_batch target column %d is out of range", column);
	}
}

static inline int
pg_batch_layout_column(const PgBatchLayout *layout, int target)
{
	Assert(target >= 0 && target < layout->ntargets);
	return layout->target_columns == NULL ? target :
		layout->target_columns[target];
}

#endif							/* PG_BATCH_LAYOUT_H */
