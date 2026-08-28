/*
 * Common operations for consumers of batches published through the bridge.
 *
 * This header deals with the common batch envelope, its row selection, and
 * Datum materialization. Borrowed physical column views live in vector.h.
 */
#ifndef PG_BATCH_RUNTIME_H
#define PG_BATCH_RUNTIME_H

#include "postgres.h"

#include "nodes/bitmapset.h"

#include "bridge.h"
#include "vector.h"

static inline int
pg_batch_row_count(const PgBatchBridgeBatch *batch)
{
	return pg_batch_bridge_selection_count(batch);
}

static inline bool
pg_batch_has_rows(const PgBatchBridgeBatch *batch)
{
	if (likely(batch->nwords == 1))
		return batch->selection[0] != 0;
	for (int word = 0; word < batch->nwords; word++)
	{
		if (batch->selection[word] != 0)
			return true;
	}
	return false;
}

/* Return the active bits in one word, excluding bits beyond batch->nrows. */
static inline uint64
pg_batch_selection_word(const PgBatchBridgeBatch *batch, int word)
{
	uint64		rows;
	int			remaining;

	Assert(word >= 0 && word < batch->nwords);
	rows = batch->selection[word];
	remaining = batch->nrows - word * 64;
	if (remaining < 64)
		rows &= UINT64_MAX >> (64 - remaining);
	return rows;
}

static inline void
pg_batch_prepare_columns(PgBatchBridgeBatch *batch, const Bitmapset *columns,
						 const uint64 *selected_rows,
						 PgBatchBridgeMaterializePhase phase)
{
	if (columns != NULL && batch->ops->prepare_columns != NULL)
		batch->ops->prepare_columns(batch, columns, selected_rows, phase);
}

static inline void
pg_batch_get_datum_column(PgBatchBridgeBatch *batch, int column,
						  const uint64 *selected_rows,
						  PgBatchBridgeMaterializePhase phase,
						  PgBatchBridgeDatumColumn *result)
{
	if (batch->ops->get_datum_column == NULL)
		elog(ERROR, "pg_batch source cannot materialize Datum columns");
	batch->ops->get_datum_column(batch, column, selected_rows, phase, result);
}

#endif							/* PG_BATCH_RUNTIME_H */
