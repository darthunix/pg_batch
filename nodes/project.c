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
	PgBatchOutput *output;
	PgBatchExprProjection *projection;
	const PgBatchRequest *request;
	List	   *input_columns;
	bool		published;
	bool		request_forwarded;
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
	state->css.slotOps = &TTSOpsVirtual;
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
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	const char *producer_name = strVal(linitial(cscan->custom_private));
	const PgBatchRequest *child_request;
	Bitmapset  *survivors;
	PgBatchLayout layout = PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);
	int			ncolumns;

	pg_batch_init_children(node, estate, eflags);
	state->child = linitial(node->custom_ps);
	state->input_columns = lsecond_node(List, cscan->custom_private);
	state->input = pg_batch_input_create(estate->es_query_cxt,
		state->child, producer_name);
	state->projection = pg_batch_expr_projection_create(
		estate->es_query_cxt, cscan->custom_exprs, &node->ss.ps,
		resolve_project_var, state->input_columns);

	ncolumns = list_length(cscan->custom_exprs);
	layout.ncolumns = ncolumns;
	layout.ntargets = ncolumns;
	state->output = pg_batch_output_create(estate->es_query_cxt,
		slot, &layout);
	state->request = pg_batch_output_request(state->output);

	child_request = pg_batch_input_request(state->input);
	survivors = bms_union(child_request->project_columns,
		pg_batch_expr_projection_input_columns(state->projection));
	pg_batch_input_set_request(state->input, child_request->filter_columns,
		survivors, true, child_request->max_rows);
	bms_free(survivors);
}

static TupleTableSlot *
project_exec(CustomScanState *node)
{
	BatchProjectState *state = (BatchProjectState *) node;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	PgBatchInputBatch input;
	PgBatch *batch;
	int			first_row;

	if (!state->request_forwarded)
	{
		const PgBatchRequest *child_request =
			pg_batch_input_request(state->input);
		int			max_rows = child_request->max_rows == 0 ?
			state->request->max_rows :
			state->request->max_rows == 0 ? child_request->max_rows :
			Min(child_request->max_rows, state->request->max_rows);

		pg_batch_input_set_request(state->input,
			child_request->filter_columns, child_request->project_columns,
			true, max_rows);
		state->request_forwarded = true;
	}

	if (!state->request->return_batch)
		elog(ERROR, "pg_batch project requires a batch-aware parent");
	if (state->published)
	{
		if (!pg_batch_output_batch_finished(state->output))
			elog(ERROR, "pg_batch parent requested a new projected batch too early");
		pg_batch_output_reset(state->output);
		pg_batch_input_finish(state->input);
		state->published = false;
	}
	if (!pg_batch_input_next(state->input, &input))
	{
		pg_batch_output_reset(state->output);
		return ExecClearTuple(slot);
	}

	ResetExprContext(node->ss.ps.ps_ExprContext);
	batch = pg_batch_expr_projection_bind(state->projection, input.batch,
									  node->ss.ps.ps_ExprContext);
	first_row = pg_batch_next_selected(batch, -1);
	if (first_row < 0)
		elog(ERROR, "pg_batch project received an empty input batch");
	pg_batch_output_publish(state->output, batch);
	state->published = true;
	state->batches++;
	state->rows += pg_batch_row_count(batch);
	if (node->ss.ps.instrument != NULL)
		node->ss.ps.instrument->tuplecount += pg_batch_row_count(batch) - 1;
	return slot;
}

static void
project_end(CustomScanState *node)
{
	BatchProjectState *state = (BatchProjectState *) node;

	pg_batch_output_reset(state->output);
	if (state->published)
		pg_batch_input_finish(state->input);
	state->published = false;
	pg_batch_end_children(node);
}

static void
project_rescan(CustomScanState *node)
{
	BatchProjectState *state = (BatchProjectState *) node;

	pg_batch_output_reset(state->output);
	if (state->published)
		pg_batch_input_finish(state->input);
	pg_batch_rescan_children(node);
	pg_batch_input_rescan(state->input);
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
