#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"

#include "internal.h"

/*
 * PgBatchPack is the row-to-batch boundary used below batch-aware nodes.
 * A scalar child may reuse its result slot on every ExecProcNode() call, so
 * all non-NULL by-reference values are copied before the next row is fetched.
 */
typedef struct BatchPackState
{
	CustomScanState css;
	PlanState  *child;
	const PgBatchBridgeRequest *request;
	PgBatchDatumBuffer *buffer;
	PgBatchBridgeBatch *active_batch;
	int			ncolumns;
	bool		published;
	uint64		packed_batches;
	uint64		packed_rows;
} BatchPackState;

static const CustomExecMethods pack_exec_methods;

static void pack_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *pack_exec(CustomScanState *node);
static void pack_end(CustomScanState *node);
static void pack_rescan(CustomScanState *node);
static void pack_explain(CustomScanState *node, List *ancestors,
						 ExplainState *es);

Node *
pg_batch_create_pack_state(CustomScan *cscan)
{
	BatchPackState *state = palloc0_object(BatchPackState);

	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &pack_exec_methods;
	state->css.slotOps = &PgBatchSlotOps;
	return (Node *) state;
}

static const CustomExecMethods pack_exec_methods = {
	.CustomName = "PgBatchPack",
	.BeginCustomScan = pack_begin,
	.ExecCustomScan = pack_exec,
	.EndCustomScan = pack_end,
	.ReScanCustomScan = pack_rescan,
	.ExplainCustomScan = pack_explain,
};

static void
pack_begin(CustomScanState *node, EState *estate, int eflags)
{
	BatchPackState *state = (BatchPackState *) node;
	PgBatchSlot *slot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);
	List	   *logical_columns = NIL;

	pg_batch_init_children(node, estate, eflags);
	state->child = linitial(node->custom_ps);
	state->ncolumns = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor->natts;
	state->buffer = pg_batch_datum_buffer_create(estate->es_query_cxt,
		ExecGetResultType(state->child), state->ncolumns, PG_BATCH_SIZE);
	for (int column = 0; column < state->ncolumns; column++)
		logical_columns = lappend_int(logical_columns, column + 1);
	pg_batch_configure_slot(slot, NULL, logical_columns, 0);
	list_free(logical_columns);
	state->request = pg_batch_bridge->get_request(slot->binding);
}

static PgBatchBridgeBatch *
fill_batch(BatchPackState *state)
{
	pg_batch_datum_buffer_reset(state->buffer);
	while (!pg_batch_datum_buffer_is_full(state->buffer))
	{
		TupleTableSlot *slot = ExecProcNode(state->child);

		if (TupIsNull(slot))
			break;
		pg_batch_datum_buffer_append_slot(state->buffer, slot);
	}
	return pg_batch_datum_buffer_finish(state->buffer, InvalidOid);
}

static TupleTableSlot *
pack_exec(CustomScanState *node)
{
	BatchPackState *state = (BatchPackState *) node;
	PgBatchSlot *slot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);
	PgBatchBridgeBatch *batch;

	if (!state->request->return_batch)
		elog(ERROR, "pg_batch pack requires a batch-aware parent");
	if (state->published)
	{
		if (!state->active_batch->consumed)
			elog(ERROR, "pg_batch parent requested a new packed batch too early");
		ExecClearTuple(&slot->base);
		state->published = false;
		state->active_batch = NULL;
	}
	batch = fill_batch(state);
	if (batch == NULL)
		return ExecClearTuple(&slot->base);

	pg_batch_bridge->publish_batch(slot->binding, batch);
	state->active_batch = batch;
	state->published = true;
	state->packed_batches++;
	state->packed_rows += batch->nrows;
	if (node->ss.ps.instrument != NULL)
		node->ss.ps.instrument->tuplecount += batch->nrows - 1;
	return &slot->base;
}

static void
pack_end(CustomScanState *node)
{
	BatchPackState *state = (BatchPackState *) node;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	pg_batch_datum_buffer_reset(state->buffer);
	state->active_batch = NULL;
	state->published = false;
	pg_batch_end_children(node);
}

static void
pack_rescan(CustomScanState *node)
{
	BatchPackState *state = (BatchPackState *) node;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	pg_batch_datum_buffer_reset(state->buffer);
	state->active_batch = NULL;
	state->published = false;
	state->packed_batches = 0;
	state->packed_rows = 0;
	pg_batch_rescan_children(node);
}

static void
pack_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	BatchPackState *state = (BatchPackState *) node;

	ExplainPropertyInteger("Batch Size", NULL, PG_BATCH_SIZE, es);
	if (es->analyze)
	{
		ExplainPropertyInteger("Packed Batches", NULL,
							   state->packed_batches, es);
		ExplainPropertyInteger("Packed Rows", NULL, state->packed_rows, es);
	}
}
