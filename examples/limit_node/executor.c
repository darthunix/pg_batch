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
	PgBatchPass *pass;
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
	if (state->css.methods != &limit_exec_methods || state->pass == NULL)
		elog(ERROR, "pg_batch_limit has no request binding");
	return pg_batch_pass_binding(state->pass);
}

static int
trim_batch(void *private_data, PgBatchInput *input,
		   PgBatchInputBatch *input_batch, int input_rows)
{
	BatchLimitState *state = private_data;
	PgBatch    *batch = input_batch->batch;
	int			remaining = 0;
	int			row = -1;

	(void) input;
	(void) input_rows;
	while ((row = pg_batch_next_selected(batch, row)) >= 0)
	{
		if (state->offset_remaining > 0)
		{
			pg_batch_clear_row(batch, row);
			state->offset_remaining--;
		}
		else if (!state->no_count && state->count_remaining == 0)
			pg_batch_clear_row(batch, row);
		else if (!state->no_count)
		{
			state->count_remaining--;
			remaining++;
		}
		else
			remaining++;
	}
	if (!state->no_count && state->count_remaining == 0)
		pg_batch_pass_stop(state->pass);
	return remaining;
}

static const PgBatchPassOps limit_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_PASS_OPS_ABI_VERSION, PgBatchPassOps),
	.process_batch = trim_batch,
};

static void
limit_begin(CustomScanState *node, EState *estate, int eflags)
{
	BatchLimitState *state = (BatchLimitState *) node;
	CustomScan *scan = castNode(CustomScan, node->ss.ps.plan);
	const char *producer_name = strVal(linitial(scan->custom_private));
	List	   *columns = lsecond_node(List, scan->custom_private);
	Bitmapset  *project_columns = NULL;
	PgBatchLayout layout = PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);
	int			position = 0;

	if (eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_batch_limit supports only forward scans")));
	Assert(list_length(scan->custom_exprs) == 2);
	layout.ncolumns = intVal(lthird(scan->custom_private));
	layout.ntargets = list_length(columns);
	layout.target_columns = palloc_array(int, layout.ntargets);
	foreach_int(column, columns)
	{
		if (column < 0 || column >= layout.ncolumns)
			elog(ERROR, "pg_batch_limit received an invalid column layout");
		project_columns = bms_add_member(project_columns, column);
		((int *) layout.target_columns)[position++] = column;
	}
	state->pass = pg_batch_pass_create(estate->es_query_cxt, node, estate,
		eflags, producer_name, &layout, NULL, project_columns, 0,
		PG_BATCH_PASS_COPY_ROW, &limit_ops, state);
	bms_free(project_columns);
	pfree((void *) layout.target_columns);
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
		pg_batch_pass_stop(state->pass);
	state->limits_ready = true;
	if (state->no_count ||
		state->count_remaining > PG_INT64_MAX - state->offset_remaining)
		ExecSetTupleBound(-1, pg_batch_pass_child(state->pass));
	else
		ExecSetTupleBound(state->offset_remaining + state->count_remaining,
			pg_batch_pass_child(state->pass));
}

static TupleTableSlot *
limit_exec(CustomScanState *node)
{
	BatchLimitState *state = (BatchLimitState *) node;

	if (!state->limits_ready)
		compute_limits(state);
	return pg_batch_pass_exec(state->pass);
}

static void
limit_end(CustomScanState *node)
{
	pg_batch_pass_end(((BatchLimitState *) node)->pass);
}

static void
limit_rescan(CustomScanState *node)
{
	BatchLimitState *state = (BatchLimitState *) node;

	pg_batch_pass_rescan(state->pass);
	state->limits_ready = false;
}

static void
limit_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	BatchLimitState *state = (BatchLimitState *) node;
	const PgBatchPassStats *stats = pg_batch_pass_stats(state->pass);

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
