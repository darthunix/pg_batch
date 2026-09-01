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

/* A reusable owner of an eagerly materialized column-major Datum batch. */
typedef struct PgBatchBuilder PgBatchBuilder;

/* Cached execution state for consuming batches from one plan child. */
typedef struct PgBatchInput PgBatchInput;
typedef struct PgBatchOutput PgBatchOutput;
typedef struct PgBatchUnary PgBatchUnary;
struct CustomScanState;
struct EState;

/*
 * Load the bridge extension when necessary and return its validated API.
 * Callers may retain the returned pointer for the lifetime of the backend.
 */
extern const PgBatchAPI *pg_batch_api_get(void);

/* Borrowed result of one successful input fetch. */
typedef struct PgBatchInputResult
{
	TupleTableSlot *slot;
	PgBatch *batch;
} PgBatchInputResult;

/* One prepared set of columns and rows from an active batch. */
typedef struct PgBatchColumnAccess
{
	PgBatch    *batch;
	const Bitmapset *columns;
	const PgBatchSelection *rows;
	PgBatchColumnPhase phase;
} PgBatchColumnAccess;

static inline int
pg_batch_row_count(const PgBatch *batch)
{
	return pg_batch_selection_count(&batch->selection);
}

static inline bool
pg_batch_has_rows(const PgBatch *batch)
{
	if (likely(batch->selection.nwords == 1))
		return batch->selection.words[0] != 0;
	for (int word = 0; word < batch->selection.nwords; word++)
	{
		if (batch->selection.words[word] != 0)
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

	Assert(word >= 0 && word < batch->selection.nwords);
	rows = batch->selection.words[word];
	remaining = batch->selection.nrows - word * 64;
	if (remaining < 64)
		rows &= UINT64_MAX >> (64 - remaining);
	return rows;
}

static inline void
pg_batch_prepare_columns(PgBatch *batch, const Bitmapset *columns,
						 const PgBatchSelection *selected_rows,
						 PgBatchColumnPhase phase)
{
	if (columns != NULL &&
		PG_BATCH_ABI_HAS_FIELD(batch->ops, PgBatchOps, prepare_columns) &&
		batch->ops->prepare_columns != NULL)
		batch->ops->prepare_columns(batch, columns, selected_rows, phase);
}

static inline void
pg_batch_get_datum_column(PgBatch *batch, int column,
						  const PgBatchSelection *selected_rows,
						  PgBatchColumnPhase phase,
						  PgBatchDatumVector *result)
{
	*result = (PgBatchDatumVector) PG_BATCH_STRUCT_INITIALIZER(
		PgBatchDatumVector);
	batch->ops->get_datum_column(batch, column, selected_rows, phase, result);
	pg_batch_check_datum_vector(result);
	if (result->values == NULL || result->isnull == NULL ||
		result->nrows < selected_rows->nrows)
		elog(ERROR, "pg_batch source returned an incomplete Datum column");
}

/* Prepare a column group once; subsequent getters are self-contained. */
static inline void
pg_batch_column_access_init(PgBatchColumnAccess *access, PgBatch *batch,
							const Bitmapset *columns,
							const PgBatchSelection *rows,
							PgBatchColumnPhase phase)
{
	if (access == NULL || batch == NULL || batch->ops == NULL || rows == NULL ||
		rows->nrows != batch->selection.nrows ||
		rows->nwords != batch->selection.nwords || rows->words == NULL)
		elog(ERROR, "pg_batch received an invalid column access request");
	access->batch = batch;
	access->columns = columns;
	access->rows = rows;
	access->phase = phase;
	pg_batch_prepare_columns(batch, columns, rows, phase);
}

static inline void
pg_batch_column_get_datum(PgBatchColumnAccess *access, int column,
						  PgBatchDatumVector *result)
{
	if (access == NULL || access->batch == NULL ||
		!bms_is_member(column, access->columns))
		elog(ERROR, "pg_batch column %d was not requested", column + 1);
	pg_batch_get_datum_column(access->batch, column, access->rows,
		access->phase, result);
}

/* Materialize the selected rows of every requested column as Datums. */
extern void pg_batch_materialize_columns(PgBatch *batch,
										 const Bitmapset *columns,
										 const PgBatchSelection *selected_rows,
										 PgBatchColumnPhase phase);

/* Discover and cache an operations-table-wide typed native interface. */
extern const PgBatchNativeInterface *pg_batch_get_native_interface(
	PgBatch *batch, const PgBatchNativeType *type);

/* Complete construction input for an owned Datum batch builder. */
typedef struct PgBatchBuilderConfig
{
	Size		struct_size;
	MemoryContext parent_context;
	/* Borrowed descriptor that must outlive the builder. */
	TupleDesc	tuple_desc;
	/* Number of leading slot attributes to copy into batch columns. */
	int			ncolumns;
	/* Maximum rows in one result; values greater than 64 are supported. */
	int			capacity;
} PgBatchBuilderConfig;

#define PG_BATCH_BUILDER_CONFIG_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchBuilderConfig, capacity)

extern PgBatchBuilder *pg_batch_builder_create(
	const PgBatchBuilderConfig *config);

/*
 * Discard copied pass-by-reference values and start an empty batch. A caller
 * must first remove any previously returned batch from its bridge binding.
 */
extern void pg_batch_builder_reset(PgBatchBuilder *builder);

/* Return true after capacity rows have been appended or the batch is sealed. */
extern bool pg_batch_builder_is_full(const PgBatchBuilder *builder);

/*
 * Append one slot, copying pass-by-reference values into builder-owned memory.
 * The slot is fully materialized through ncolumns before it may be reused.
 */
extern void pg_batch_builder_append_slot(PgBatchBuilder *builder,
										  TupleTableSlot *slot);

/*
 * Seal and return the current batch, or NULL when the builder is empty. The
 * returned batch and its column views remain valid until reset().
 */
extern PgBatch *pg_batch_builder_finish(
	PgBatchBuilder *builder, Oid table_oid);

/*
 * Wrap a child registered as node_name. The input does not own child or
 * initialize it. The node and its request binding must remain registered
 * for the lifetime of the input.
 */
extern PgBatchInput *pg_batch_input_create(
	MemoryContext parent_context, struct PlanState *child,
	const char *node_name);

/* Return the immutable logical layout advertised by the child node. */
extern const PgBatchLayout *pg_batch_input_layout(PgBatchInput *input);

/* Return the stable binding through which the child request is configured. */
extern PgBatchBinding *pg_batch_input_request_binding(
	PgBatchInput *input);
extern const PgBatchRequest *pg_batch_input_request(PgBatchInput *input);

/* Replace the request sent to the child node before execution starts. */
extern void pg_batch_input_set_request(PgBatchInput *input,
	const PgBatchRequestSpec *spec);

/*
 * Fetch the next child result and resolve its active batch. A forwarding
 * node may return a slot different from its request binding's slot.
 * The returned pointers remain owned by the child and bridge.
 */
extern bool pg_batch_input_next(PgBatchInput *input,
	PgBatchInputResult *result);

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
extern bool pg_batch_input_finished(PgBatchInput *input);

/* Mark the current batch consumed before a later next() call. */
extern void pg_batch_input_finish(PgBatchInput *input);

/*
 * Forget cached execution pointers after the caller has rescanned the child.
 * The child is responsible for releasing any batch that was still active.
 */
extern void pg_batch_input_rescan(PgBatchInput *input);

/*
 * Callbacks for a node that changes row selection but preserves the input
 * batch representation. process may clear selection bits and returns the
 * remaining row count. A NULL process callback forwards the selection
 * unchanged. A callback that has produced its final batch calls
 * pg_batch_unary_stop().
 */
typedef int (*PgBatchUnaryProcess) (void *private_data, PgBatchInput *input,
	PgBatchInputResult *result, int input_rows);

typedef enum PgBatchUnaryRowMode
{
	/* Return the selected input slot, applying the node's projection if any. */
	PG_BATCH_UNARY_INPUT_ROW,
	/* Copy columns mapped by layout into the node's virtual scan slot. */
	PG_BATCH_UNARY_COPY_ROW
} PgBatchUnaryRowMode;

typedef struct PgBatchUnaryStats
{
	uint64		input_batches;
	uint64		input_rows;
	uint64		output_rows;
} PgBatchUnaryStats;

/* Complete construction input for a one-child selection-preserving node. */
typedef struct PgBatchUnaryConfig
{
	Size		struct_size;
	MemoryContext parent_context;
	struct CustomScanState *node;
	struct EState *estate;
	int			eflags;
	const char *child_name;
	const PgBatchLayout *layout;
	const Bitmapset *filter_columns;
	const Bitmapset *project_columns;
	int			max_rows;
	PgBatchUnaryRowMode row_mode;
	PgBatchUnaryProcess process;
	void	   *private_data;
} PgBatchUnaryConfig;

#define PG_BATCH_UNARY_CONFIG_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchUnaryConfig, private_data)

/*
 * Initialize the one child and attach a request slot described by layout.
 * The local column masks and max_rows are combined with the parent's request
 * on first execution. row_mode chooses whether scalar output borrows the
 * child's slot or copies mapped columns into the CustomScan slot.
 */
extern PgBatchUnary *pg_batch_unary_create(const PgBatchUnaryConfig *config);
extern PgBatchBinding *pg_batch_unary_binding(PgBatchUnary *unary);
extern struct PlanState *pg_batch_unary_child(PgBatchUnary *unary);
extern const PgBatchUnaryStats *pg_batch_unary_stats(PgBatchUnary *unary);
/* Stop before another child batch is fetched; the active batch stays valid. */
extern void pg_batch_unary_stop(PgBatchUnary *unary);
extern TupleTableSlot *pg_batch_unary_exec(PgBatchUnary *unary);
extern void pg_batch_unary_end(PgBatchUnary *unary);
extern void pg_batch_unary_rescan(PgBatchUnary *unary);

/*
 * Attach a bridge binding and a Datum scalar adapter to a virtual output
 * slot. layout maps every slot attribute to its compact batch column;
 * additional batch columns may represent resjunk plan entries.
 */
extern PgBatchOutput *pg_batch_output_create(
	MemoryContext parent_context, TupleTableSlot *slot,
	const PgBatchLayout *layout);

/* Return the binding used by parents to configure this node's request. */
extern PgBatchBinding *pg_batch_output_binding(PgBatchOutput *output);

/* Return the current bridge-owned request. */
extern const PgBatchRequest *pg_batch_output_request(PgBatchOutput *output);
extern void pg_batch_output_set_request(PgBatchOutput *output,
	const PgBatchRequestSpec *spec);
extern bool pg_batch_output_finished(PgBatchOutput *output);

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
extern void pg_batch_output_clear(PgBatchOutput *output);

#endif							/* PG_BATCH_RUNTIME_H */
