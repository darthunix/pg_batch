#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "utils/errcodes.h"

#include "internal.h"

typedef struct BatchLimitState
{
	CustomScanState css;
	PgBatchUnary *unary;
	ExprState  *offset_expr;
	ExprState  *count_expr;
	int64		offset_remaining;
	int64		count_remaining;
	bool		no_count;
	bool		limits_ready;
} BatchLimitState;

static const CustomExecMethods limit_exec_methods;

Node *
pg_batch_limit_create_state(CustomScan *scan)
{
	BatchLimitState *state = palloc0_object(BatchLimitState);

	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &limit_exec_methods;
	state->css.slotOps = &TTSOpsVirtual;
	return (Node *) state;
}

PgBatchBinding *
pg_batch_limit_request_binding(PlanState *planstate)
{
	BatchLimitState *state;

	if (!IsA(planstate, CustomScanState))
		elog(ERROR, "pg_batch_limit expected a custom plan state");
	state = (BatchLimitState *) planstate;
	if (state->css.methods != &limit_exec_methods || state->unary == NULL)
		elog(ERROR, "pg_batch_limit has no request binding");
	return pg_batch_unary_binding(state->unary);
}

static int
trim_batch(void *private_data, PgBatchInput *input,
		   PgBatchInputResult *input_batch, int input_rows)
{
	BatchLimitState *state = private_data;
	PgBatch    *batch = input_batch->batch;
	int			remaining = 0;
	int			row = -1;

	(void) input;
	(void) input_rows;
	while ((row = pg_batch_selection_next(&batch->selection, row)) >= 0)
	{
		if (state->offset_remaining > 0)
		{
			pg_batch_selection_clear(&batch->selection, row);
			state->offset_remaining--;
		}
		else if (!state->no_count && state->count_remaining == 0)
			pg_batch_selection_clear(&batch->selection, row);
		else if (!state->no_count)
		{
			state->count_remaining--;
			remaining++;
		}
		else
			remaining++;
	}
	if (!state->no_count && state->count_remaining == 0)
		pg_batch_unary_stop(state->unary);
	return remaining;
}

static void
limit_begin(CustomScanState *node, EState *estate, int eflags)
{
	BatchLimitState *state = (BatchLimitState *) node;
	CustomScan *scan = castNode(CustomScan, node->ss.ps.plan);
	PgBatchPlanInfo plan = PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanInfo);
	Bitmapset  *project_columns = NULL;
	PgBatchUnaryConfig config;

	if (eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("pg_batch_limit supports only forward scans")));
	Assert(list_length(scan->custom_exprs) == 2);
	pg_batch_plan_get_info(scan, &plan);
	if (plan.nchildren != 1 || plan.child_names[0] == NULL)
		elog(ERROR, "pg_batch_limit expected one batch-aware child");
	for (int target = 0; target < plan.layout.ntargets; target++)
	{
		int			column = pg_batch_layout_column(&plan.layout, target);

		project_columns = bms_add_member(project_columns, column);
	}
	config = (PgBatchUnaryConfig) {
		.struct_size = sizeof(config),
		.parent_context = estate->es_query_cxt,
		.node = node,
		.estate = estate,
		.eflags = eflags,
		.child_name = plan.child_names[0],
		.layout = &plan.layout,
		.project_columns = project_columns,
		.row_mode = PG_BATCH_UNARY_COPY_ROW,
		.process = trim_batch,
		.private_data = state,
	};
	state->unary = pg_batch_unary_create(&config);
	bms_free(project_columns);
	pfree(plan.child_names);
	if (plan.layout.target_columns != NULL)
		pfree((void *) plan.layout.target_columns);
	if (linitial(scan->custom_exprs) != NULL)
		state->offset_expr = ExecInitExpr(linitial(scan->custom_exprs),
			&node->ss.ps);
	if (lsecond(scan->custom_exprs) != NULL)
		state->count_expr = ExecInitExpr(lsecond(scan->custom_exprs),
			&node->ss.ps);
}

static void
compute_limits(BatchLimitState *state)
{
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	Datum		value;
	bool		isnull;

	state->offset_remaining = 0;
	if (state->offset_expr != NULL)
	{
		value = ExecEvalExprSwitchContext(state->offset_expr, econtext, &isnull);
		state->offset_remaining = isnull ? 0 : DatumGetInt64(value);
		if (state->offset_remaining < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ROW_COUNT_IN_RESULT_OFFSET_CLAUSE),
					 errmsg("OFFSET must not be negative")));
	}
	state->no_count = state->count_expr == NULL;
	state->count_remaining = 0;
	if (state->count_expr != NULL)
	{
		value = ExecEvalExprSwitchContext(state->count_expr, econtext, &isnull);
		state->no_count = isnull;
		state->count_remaining = isnull ? 0 : DatumGetInt64(value);
		if (state->count_remaining < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ROW_COUNT_IN_LIMIT_CLAUSE),
					 errmsg("LIMIT must not be negative")));
	}
	if (!state->no_count && state->count_remaining == 0)
		pg_batch_unary_stop(state->unary);
	state->limits_ready = true;
	if (state->no_count ||
		state->count_remaining > PG_INT64_MAX - state->offset_remaining)
		ExecSetTupleBound(-1, pg_batch_unary_child(state->unary));
	else
		ExecSetTupleBound(state->offset_remaining + state->count_remaining,
			pg_batch_unary_child(state->unary));
}

static TupleTableSlot *
limit_exec(CustomScanState *node)
{
	BatchLimitState *state = (BatchLimitState *) node;

	if (!state->limits_ready)
		compute_limits(state);
	return pg_batch_unary_exec(state->unary);
}

static void
limit_end(CustomScanState *node)
{
	pg_batch_unary_end(((BatchLimitState *) node)->unary);
}

static void
limit_rescan(CustomScanState *node)
{
	BatchLimitState *state = (BatchLimitState *) node;

	pg_batch_unary_rescan(state->unary);
	state->limits_ready = false;
}

static void
limit_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	BatchLimitState *state = (BatchLimitState *) node;
	const PgBatchUnaryStats *stats = pg_batch_unary_stats(state->unary);

	if (es->analyze)
	{
		ExplainPropertyInteger("Input Batches", NULL, stats->input_batches, es);
		ExplainPropertyInteger("Input Rows", NULL, stats->input_rows, es);
		ExplainPropertyInteger("Output Rows", NULL, stats->output_rows, es);
	}
}

static const CustomExecMethods limit_exec_methods = {
	.CustomName = "PgBatchLimit",
	.BeginCustomScan = limit_begin,
	.ExecCustomScan = limit_exec,
	.EndCustomScan = limit_end,
	.ReScanCustomScan = limit_rescan,
	.ExplainCustomScan = limit_explain,
};
