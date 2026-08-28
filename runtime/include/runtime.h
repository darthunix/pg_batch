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

#include "arrow.h"
#include "bridge.h"
#include "vector.h"

/* A reusable owner of an eagerly materialized column-major Datum batch. */
typedef struct PgBatchDatumBuffer PgBatchDatumBuffer;

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

/* Materialize the selected rows of every requested column as Datums. */
extern void pg_batch_materialize_columns(PgBatchBridgeBatch *batch,
										 const Bitmapset *columns,
										 const uint64 *selected_rows,
										 PgBatchBridgeMaterializePhase phase);

/* Return a prepared Arrow column when the active batch publishes one. */
extern bool pg_batch_get_arrow_column(PgBatchBridgeBatch *batch, int column,
									  PgBatchBridgeArrowView *result);

/*
 * Return an int4 view without converting native packed values to Datums.
 * The caller must first prepare the requested rows and column.
 */
extern void pg_batch_get_int4_vector(PgBatchBridgeBatch *batch, int column,
									 const uint64 *selected_rows,
									 PgBatchBridgeMaterializePhase phase,
									 PgBatchInt4Vector *result);

/*
 * Create a row-to-batch buffer in parent_context. tuple_desc is borrowed and
 * must outlive the buffer. The first ncolumns attributes are copied from each
 * appended slot, whose descriptor must be binary-compatible with tuple_desc.
 * capacity may be larger than 64 rows.
 */
extern PgBatchDatumBuffer *pg_batch_datum_buffer_create(
	MemoryContext parent_context, TupleDesc tuple_desc,
	int ncolumns, int capacity);

/*
 * Discard copied pass-by-reference values and start an empty batch. A caller
 * must first remove any previously returned batch from its bridge binding.
 */
extern void pg_batch_datum_buffer_reset(PgBatchDatumBuffer *buffer);

/* Return true after capacity rows have been appended or the batch is sealed. */
extern bool pg_batch_datum_buffer_is_full(const PgBatchDatumBuffer *buffer);

/*
 * Append one slot, copying pass-by-reference values into buffer-owned memory.
 * The slot is fully materialized through ncolumns before it may be reused.
 */
extern void pg_batch_datum_buffer_append_slot(PgBatchDatumBuffer *buffer,
										  TupleTableSlot *slot);

/*
 * Seal and return the current batch, or NULL when the buffer is empty. The
 * returned batch and its column views remain valid until reset().
 */
extern PgBatchBridgeBatch *pg_batch_datum_buffer_finish(
	PgBatchDatumBuffer *buffer, Oid table_oid);

#endif							/* PG_BATCH_RUNTIME_H */
