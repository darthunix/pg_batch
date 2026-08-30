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

/* Cached execution state for consuming batches from one plan child. */
typedef struct PgBatchInput PgBatchInput;
typedef struct PgBatchOutput PgBatchOutput;

/*
 * Load the bridge extension when necessary and return its validated API.
 * Callers may retain the returned pointer for the lifetime of the backend.
 */
extern const PgBatchAPI *pg_batch_api_get(void);

/* Borrowed result of one successful input fetch. */
typedef struct PgBatchInputBatch
{
	TupleTableSlot *slot;
	PgBatch *batch;
} PgBatchInputBatch;

static inline int
pg_batch_row_count(const PgBatch *batch)
{
	return pg_batch_selection_count(batch);
}

static inline bool
pg_batch_has_rows(const PgBatch *batch)
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
pg_batch_selection_word(const PgBatch *batch, int word)
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
pg_batch_prepare_columns(PgBatch *batch, const Bitmapset *columns,
						 const uint64 *selected_rows,
						 PgBatchColumnPhase phase)
{
	if (columns != NULL && batch->ops->prepare_columns != NULL)
		batch->ops->prepare_columns(batch, columns, selected_rows, phase);
}

static inline void
pg_batch_get_datum_column(PgBatch *batch, int column,
						  const uint64 *selected_rows,
						  PgBatchColumnPhase phase,
						  PgBatchDatumVector *result)
{
	if (batch->ops->get_datum_column == NULL)
		elog(ERROR, "pg_batch source cannot materialize Datum columns");
	batch->ops->get_datum_column(batch, column, selected_rows, phase, result);
}

/* Materialize the selected rows of every requested column as Datums. */
extern void pg_batch_materialize_columns(PgBatch *batch,
										 const Bitmapset *columns,
										 const uint64 *selected_rows,
										 PgBatchColumnPhase phase);

/* Return a prepared Arrow column when the active batch publishes one. */
extern bool pg_batch_get_arrow_column(PgBatch *batch, int column,
									  PgBatchArrowView *result);

/*
 * Return an int4 view without converting native packed values to Datums.
 * The caller must first prepare the requested rows and column.
 */
extern void pg_batch_get_int4_vector(PgBatch *batch, int column,
									 const uint64 *selected_rows,
									 PgBatchColumnPhase phase,
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
extern PgBatch *pg_batch_datum_buffer_finish(
	PgBatchDatumBuffer *buffer, Oid table_oid);

/*
 * Wrap a child registered as producer_name. The input does not own child or
 * initialize it. The producer and its request binding must remain registered
 * for the lifetime of the input.
 */
extern PgBatchInput *pg_batch_input_create(
	MemoryContext parent_context, const PgBatchAPI *api,
	struct PlanState *child, const char *producer_name);

/* Return the stable binding through which the child request is configured. */
extern PgBatchBinding *pg_batch_input_request_binding(
	PgBatchInput *input);

/*
 * Fetch the next child result and resolve its active batch. A pass-through
 * producer may return a slot different from its request binding's slot.
 * The returned pointers remain owned by the child and bridge.
 */
extern bool pg_batch_input_next(PgBatchInput *input,
	PgBatchInputBatch *result);

/* Mark the current batch consumed before a later next() call. */
extern void pg_batch_input_finish(PgBatchInput *input);

/*
 * Forget cached execution pointers after the caller has rescanned the child.
 * The child is responsible for releasing any batch that was still active.
 */
extern void pg_batch_input_rescan(PgBatchInput *input);

/*
 * Attach a bridge binding and a Datum scalar adapter to a virtual output
 * slot. The first slot attributes correspond to batch columns in order;
 * additional batch columns may represent resjunk plan entries.
 * source_attnums describes the full layout advertised through PgBatchRequest.
 */
extern PgBatchOutput *pg_batch_output_create(
	MemoryContext parent_context, const PgBatchAPI *api,
	TupleTableSlot *slot, const AttrNumber *source_attnums, int ncolumns);

/* Return the binding used by parents to configure this node's request. */
extern PgBatchBinding *pg_batch_output_binding(PgBatchOutput *output);

/* Return the current bridge-owned request. */
extern const PgBatchRequest *pg_batch_output_request(PgBatchOutput *output);

/*
 * Publish a batch and return the nonempty output slot. If a previous batch is
 * active, its consumer must have finished it before this call.
 */
extern TupleTableSlot *pg_batch_output_publish(PgBatchOutput *output,
	PgBatch *batch);

/* Select another row for a scalar parent without changing the active batch. */
extern TupleTableSlot *pg_batch_output_select(PgBatchOutput *output, int row);

/* Finish the active batch on behalf of a scalar consumer. */
extern void pg_batch_output_finish(PgBatchOutput *output);

/* Release any active batch and forget tuple state during end or rescan. */
extern void pg_batch_output_reset(PgBatchOutput *output);

#endif							/* PG_BATCH_RUNTIME_H */
