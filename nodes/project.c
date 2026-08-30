#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"

#include "internal.h"

typedef struct BatchProjectState
{
	CustomScanState css;
	PlanState  *child;
	PgBatchInput *input;
	PgBatchExprProjection *projection;
	const PgBatchBridgeRequest *request;
	PgBatchBridgeBatch *active_batch;
	List	   *input_columns;
	bool		published;
	uint64	batches;
	uint64	rows;
} BatchProjectState;

static const CustomExecMethods project_exec_methods;

Node *
pg_batch_create_project_state(CustomScan *cscan)
{
	BatchProjectState *state = palloc0_object(BatchProjectState);

	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &project_exec_methods;
	state->css.slotOps = &PgBatchSlotOps;
	return (Node *) state;
}

static int
resolve_project_var(const Var *var, void *context)
{
	List	   *input_columns = context;
	int			position = var->varattno - 1;

	if (position < 0 || position >= list_length(input_columns))
		return -1;
	return list_nth_int(input_columns, position);
}

static void
project_begin(CustomScanState *node, EState *estate, int eflags)
{
	BatchProjectState *state = (BatchProjectState *) node;
	CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
	PgBatchSlot *slot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);
	const char *producer_name = strVal(linitial(cscan->custom_private));
	PgBatchBridgeBinding *child_binding;
	const PgBatchBridgeRequest *child_request;
	Bitmapset  *survivors;
	List	   *logical_columns = NIL;

	pg_batch_init_children(node, estate, eflags);
	state->child = linitial(node->custom_ps);
	state->input_columns = lsecond_node(List, cscan->custom_private);
	state->input = pg_batch_input_create(estate->es_query_cxt,
		pg_batch_bridge, state->child, producer_name);
	state->projection = pg_batch_expr_projection_create(
		estate->es_query_cxt, cscan->custom_exprs, &node->ss.ps,
		resolve_project_var, state->input_columns);

	for (int column = 0; column < list_length(cscan->custom_exprs); column++)
		logical_columns = lappend_int(logical_columns, column + 1);
	pg_batch_configure_slot(slot, NULL, logical_columns, 0);
	list_free(logical_columns);
	state->request = pg_batch_bridge->get_request(slot->binding);

	child_binding = pg_batch_input_request_binding(state->input);
	child_request = pg_batch_bridge->get_request(child_binding);
	survivors = bms_union(child_request->survivor_columns,
		pg_batch_expr_projection_input_columns(state->projection));
	pg_batch_bridge->set_request(child_binding, child_request->filter_columns,
							 survivors, true);
	bms_free(survivors);
}

static TupleTableSlot *
project_exec(CustomScanState *node)
{
	BatchProjectState *state = (BatchProjectState *) node;
	PgBatchSlot *slot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);
	PgBatchInputBatch input;
	PgBatchBridgeBatch *batch;
	int			first_row;

	if (!state->request->return_batch)
		elog(ERROR, "pg_batch project requires a batch-aware parent");
	if (state->published)
	{
		if (!state->active_batch->consumed)
			elog(ERROR, "pg_batch parent requested a new projected batch too early");
		ExecClearTuple(&slot->base);
		state->published = false;
		state->active_batch = NULL;
	}
	if (!pg_batch_input_advance(state->input, &input))
		return ExecClearTuple(&slot->base);

	ResetExprContext(node->ss.ps.ps_ExprContext);
	batch = pg_batch_expr_projection_bind(state->projection, input.batch,
									  node->ss.ps.ps_ExprContext);
	pg_batch_bridge->publish_batch(slot->binding, batch);
	first_row = pg_batch_bridge_next_selected(batch, -1);
	if (first_row < 0)
		elog(ERROR, "pg_batch project received an empty input batch");
	pg_batch_select_row(&slot->base, first_row);
	state->active_batch = batch;
	state->published = true;
	state->batches++;
	state->rows += pg_batch_row_count(batch);
	if (node->ss.ps.instrument != NULL)
		node->ss.ps.instrument->tuplecount += pg_batch_row_count(batch) - 1;
	return &slot->base;
}

static void
project_end(CustomScanState *node)
{
	BatchProjectState *state = (BatchProjectState *) node;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	if (state->published)
		pg_batch_input_finish(state->input);
	state->active_batch = NULL;
	state->published = false;
	pg_batch_end_children(node);
}

static void
project_rescan(CustomScanState *node)
{
	BatchProjectState *state = (BatchProjectState *) node;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	if (state->published)
		pg_batch_input_finish(state->input);
	pg_batch_rescan_children(node);
	pg_batch_input_reset(state->input);
	state->active_batch = NULL;
	state->published = false;
	state->batches = 0;
	state->rows = 0;
}

static void
project_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	BatchProjectState *state = (BatchProjectState *) node;

	ExplainPropertyInteger("Expressions", NULL,
		list_length(castNode(CustomScan, node->ss.ps.plan)->custom_exprs), es);
	if (es->analyze)
	{
		ExplainPropertyInteger("Batches", NULL, state->batches, es);
		ExplainPropertyInteger("Rows", NULL, state->rows, es);
	}
}

static const CustomExecMethods project_exec_methods = {
	.CustomName = "PgBatchProject",
	.BeginCustomScan = project_begin,
	.ExecCustomScan = project_exec,
	.EndCustomScan = project_end,
	.ReScanCustomScan = project_rescan,
	.ExplainCustomScan = project_explain,
};
