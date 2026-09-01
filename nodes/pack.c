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
	PgBatchOutput *output;
	const PgBatchRequest *request;
	PgBatchBuilder *builder;
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
	state->css.slotOps = &TTSOpsVirtual;
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
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	PgBatchLayout layout = PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);

	pg_batch_init_children(node, estate, eflags);
	state->child = linitial(node->custom_ps);
	state->ncolumns = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor->natts;
	state->builder = pg_batch_builder_create(
		&(PgBatchBuilderConfig) {
			.struct_size = sizeof(PgBatchBuilderConfig),
			.parent_context = estate->es_query_cxt,
			.tuple_desc = ExecGetResultType(state->child),
			.ncolumns = state->ncolumns,
			.capacity = PG_BATCH_SIZE
		});
	layout.ncolumns = state->ncolumns;
	layout.ntargets = state->ncolumns;
	state->output = pg_batch_output_create(estate->es_query_cxt,
		slot, &layout);
	state->request = pg_batch_output_request(state->output);
}

static PgBatch *
fill_batch(BatchPackState *state)
{
	int			limit = state->request->spec.max_rows > 0 ?
		Min(state->request->spec.max_rows, PG_BATCH_SIZE) : PG_BATCH_SIZE;
	int			nrows = 0;

	pg_batch_builder_reset(state->builder);
	while (nrows < limit && !pg_batch_builder_is_full(state->builder))
	{
		TupleTableSlot *slot = ExecProcNode(state->child);

		if (TupIsNull(slot))
			break;
		pg_batch_builder_append_slot(state->builder, slot);
		nrows++;
	}
	return pg_batch_builder_finish(state->builder, InvalidOid);
}

static TupleTableSlot *
pack_exec(CustomScanState *node)
{
	BatchPackState *state = (BatchPackState *) node;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	PgBatch *batch;

	if (state->request->spec.output_mode == PG_BATCH_OUTPUT_ROWS)
		elog(ERROR, "pg_batch pack requires a batch-aware parent");
	if (state->published)
	{
		if (!pg_batch_api->finished(
				pg_batch_output_binding(state->output)))
			elog(ERROR, "pg_batch parent requested a new packed batch too early");
		pg_batch_output_clear(state->output);
		state->published = false;
	}
	batch = fill_batch(state);
	if (batch == NULL)
	{
		pg_batch_output_clear(state->output);
		return ExecClearTuple(slot);
	}

	pg_batch_output_publish(state->output, batch);
	state->published = true;
	state->packed_batches++;
	state->packed_rows += batch->selection.nrows;
	if (node->ss.ps.instrument != NULL)
		node->ss.ps.instrument->tuplecount += batch->selection.nrows - 1;
	return slot;
}

static void
pack_end(CustomScanState *node)
{
	BatchPackState *state = (BatchPackState *) node;

	pg_batch_output_clear(state->output);
	pg_batch_builder_reset(state->builder);
	state->published = false;
	pg_batch_end_children(node);
}

static void
pack_rescan(CustomScanState *node)
{
	BatchPackState *state = (BatchPackState *) node;

	pg_batch_output_clear(state->output);
	pg_batch_builder_reset(state->builder);
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
