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
typedef struct PgBatchPass PgBatchPass;
struct CustomScanState;
struct EState;

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
	if (columns != NULL &&
		PG_BATCH_ABI_HAS_FIELD(batch->ops, PgBatchOps, prepare_columns) &&
		batch->ops->prepare_columns != NULL)
		batch->ops->prepare_columns(batch, columns, selected_rows, phase);
}

static inline void
pg_batch_get_datum_column(PgBatch *batch, int column,
						  const uint64 *selected_rows,
						  PgBatchColumnPhase phase,
						  PgBatchDatumVector *result)
{
	*result = (PgBatchDatumVector) PG_BATCH_STRUCT_INITIALIZER(
		PgBatchDatumVector);
	batch->ops->get_datum_column(batch, column, selected_rows, phase, result);
}

/* Materialize the selected rows of every requested column as Datums. */
extern void pg_batch_materialize_columns(PgBatch *batch,
										 const Bitmapset *columns,
										 const uint64 *selected_rows,
										 PgBatchColumnPhase phase);

/* Discover and cache an operations-table-wide typed native interface. */
extern const PgBatchNativeInterface *pg_batch_get_native_interface(
	PgBatch *batch, const PgBatchNativeType *type);

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
	MemoryContext parent_context, struct PlanState *child,
	const char *producer_name);

/* Return the immutable logical layout advertised by the child producer. */
extern const PgBatchLayout *pg_batch_input_layout(PgBatchInput *input);

/* Return the stable binding through which the child request is configured. */
extern PgBatchBinding *pg_batch_input_request_binding(
	PgBatchInput *input);
extern const PgBatchRequest *pg_batch_input_request(PgBatchInput *input);

/* Replace the request sent to the child producer before execution starts. */
extern void pg_batch_input_set_request(PgBatchInput *input,
	const Bitmapset *filter_columns, const Bitmapset *project_columns,
	bool return_batch, int max_rows);

/*
 * Fetch the next child result and resolve its active batch. A pass-through
 * producer may return a slot different from its request binding's slot.
 * The returned pointers remain owned by the child and bridge.
 */
extern bool pg_batch_input_next(PgBatchInput *input,
	PgBatchInputBatch *result);

/* Select one row of the active input and return its scalar slot view. */
extern TupleTableSlot *pg_batch_input_select_row(PgBatchInput *input,
	int row);

/* Return the active borrowed fast row view, or NULL with an older bridge. */
extern const PgBatchRowView *pg_batch_input_row_view(PgBatchInput *input);

/*
 * Select one active row and expose mapped batch columns in a virtual slot.
 * batch_columns has one entry for every destination attribute.
 */
extern TupleTableSlot *pg_batch_input_copy_row(PgBatchInput *input,
	int row, TupleTableSlot *destination, const int *batch_columns);

/* Report whether the consumer of a forwarded active batch has finished it. */
extern bool pg_batch_input_batch_finished(PgBatchInput *input);

/* Mark the current batch consumed before a later next() call. */
extern void pg_batch_input_finish(PgBatchInput *input);

/*
 * Forget cached execution pointers after the caller has rescanned the child.
 * The child is responsible for releasing any batch that was still active.
 */
extern void pg_batch_input_rescan(PgBatchInput *input);

/*
 * Callbacks for a node that changes row selection but preserves the input
 * batch representation. process_batch may clear selection bits and returns
 * the remaining row count. A NULL process_batch forwards the selection
 * unchanged. A callback that has produced its final batch calls
 * pg_batch_pass_stop().
 */
typedef struct PgBatchPassOps
{
	uint32		abi_version;
	Size		struct_size;
	int			(*process_batch) (void *private_data, PgBatchInput *input,
								  PgBatchInputBatch *batch, int input_rows);
} PgBatchPassOps;

#define PG_BATCH_PASS_OPS_ABI_VERSION 1
#define PG_BATCH_PASS_OPS_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchPassOps, process_batch)

typedef enum PgBatchPassRowMode
{
	/* Return the selected input slot, applying the node's projection if any. */
	PG_BATCH_PASS_INPUT_ROW,
	/* Copy columns mapped by layout into the node's virtual scan slot. */
	PG_BATCH_PASS_COPY_ROW
} PgBatchPassRowMode;

typedef struct PgBatchPassStats
{
	uint64		input_batches;
	uint64		input_rows;
	uint64		output_rows;
} PgBatchPassStats;

/*
 * Initialize the one child and attach a request slot described by layout.
 * The local column masks and max_rows are combined with the parent's request
 * on first execution. row_mode chooses whether scalar output borrows the
 * child's slot or copies mapped columns into the CustomScan slot.
 */
extern PgBatchPass *pg_batch_pass_create(
	MemoryContext parent_context, struct CustomScanState *node,
	struct EState *estate, int eflags, const char *producer_name,
	const PgBatchLayout *layout, const Bitmapset *filter_columns,
	const Bitmapset *project_columns, int max_rows, PgBatchPassRowMode row_mode,
	const PgBatchPassOps *ops, void *private_data);
extern PgBatchBinding *pg_batch_pass_binding(PgBatchPass *pass);
extern struct PlanState *pg_batch_pass_child(PgBatchPass *pass);
extern const PgBatchPassStats *pg_batch_pass_stats(PgBatchPass *pass);
/* Stop before another child batch is fetched; the active batch stays valid. */
extern void pg_batch_pass_stop(PgBatchPass *pass);
extern TupleTableSlot *pg_batch_pass_exec(PgBatchPass *pass);
extern void pg_batch_pass_end(PgBatchPass *pass);
extern void pg_batch_pass_rescan(PgBatchPass *pass);

/*
 * Attach a bridge binding and a Datum scalar adapter to a virtual output
 * slot. The first slot attributes correspond to batch columns in order;
 * additional batch columns may represent resjunk plan entries. layout maps
 * the slot's target attributes to those columns.
 */
extern PgBatchOutput *pg_batch_output_create(
	MemoryContext parent_context, TupleTableSlot *slot,
	const PgBatchLayout *layout);

/* Return the binding used by parents to configure this node's request. */
extern PgBatchBinding *pg_batch_output_binding(PgBatchOutput *output);

/* Return the current bridge-owned request. */
extern const PgBatchRequest *pg_batch_output_request(PgBatchOutput *output);
extern void pg_batch_output_set_request(PgBatchOutput *output,
	const Bitmapset *filter_columns, const Bitmapset *project_columns,
	bool return_batch, int max_rows);
extern bool pg_batch_output_batch_finished(PgBatchOutput *output);

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
