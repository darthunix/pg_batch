#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "utils/errcodes.h"

#include "internal.h"

typedef struct BatchLimitState
{
	CustomScanState css;
	PlanState  *child;
	PgBatchInput *input;
	PgBatchRequestPort *request_port;
	const PgBatchRequest *request;
	int		   *batch_columns;
	ExprState  *offset_expr;
	ExprState  *count_expr;
	int64		offset_remaining;
	int64		count_remaining;
	bool		no_count;
	bool		limits_ready;
	bool		request_forwarded;
	bool		window_complete;
	TupleTableSlot *active_slot;
	PgBatch    *active_batch;
	int			next_row;
	uint64		input_batches;
	uint64		input_rows;
	uint64		output_rows;
} BatchLimitState;

static void limit_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *limit_exec(CustomScanState *node);
static void limit_end(CustomScanState *node);
static void limit_rescan(CustomScanState *node);
static void limit_explain(CustomScanState *node, List *ancestors,
	ExplainState *es);

static const CustomExecMethods limit_exec_methods = {
	.CustomName = "PgBatchLimit",
	.BeginCustomScan = limit_begin,
	.ExecCustomScan = limit_exec,
	.EndCustomScan = limit_end,
	.ReScanCustomScan = limit_rescan,
	.ExplainCustomScan = limit_explain,
};

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
	if (state->css.methods != &limit_exec_methods ||
		state->request_port == NULL)
		elog(ERROR, "pg_batch_limit has no request binding");
	return pg_batch_request_port_binding(state->request_port);
}

static Bitmapset *
layout_columns(List *items, int *ncolumns)
{
	Bitmapset  *columns = NULL;
	int			maximum = -1;

	foreach_int(column, items)
	{
		if (column < 0)
			continue;
		columns = bms_add_member(columns, column);
		maximum = Max(maximum, column);
	}
	*ncolumns = maximum + 1;
	return columns;
}

static void
limit_begin(CustomScanState *node, EState *estate, int eflags)
{
	BatchLimitState *state = (BatchLimitState *) node;
	CustomScan *scan = castNode(CustomScan, node->ss.ps.plan);
	Plan	   *child_plan;
	const char *producer_name;
	List	   *columns;
	Bitmapset  *project_columns;
	AttrNumber *source_attnums;
	int			ncolumns;
	int			request_columns;
	int			position = 0;

	if (eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_batch_limit supports only forward scans")));
	Assert(list_length(scan->custom_plans) == 1);
	Assert(list_length(scan->custom_private) == 2);
	Assert(list_length(scan->custom_exprs) == 2);

	child_plan = linitial(scan->custom_plans);
	state->child = ExecInitNode(child_plan, estate, eflags);
	node->custom_ps = list_make1(state->child);
	producer_name = strVal(linitial(scan->custom_private));
	columns = lsecond_node(List, scan->custom_private);
	project_columns = layout_columns(columns, &ncolumns);
	if (list_length(columns) !=
		node->ss.ss_ScanTupleSlot->tts_tupleDescriptor->natts)
		elog(ERROR, "pg_batch_limit received an invalid output layout");
	state->batch_columns = palloc_array(int, list_length(columns));
	foreach_int(column, columns)
	{
		if (column < 0)
			elog(ERROR, "pg_batch_limit cannot pass through a missing column");
		state->batch_columns[position++] = column;
	}
	request_columns = Max(ncolumns,
		node->ss.ss_ScanTupleSlot->tts_tupleDescriptor->natts);
	source_attnums = palloc_array(AttrNumber, request_columns);
	for (int column = 0; column < request_columns; column++)
		source_attnums[column] = column + 1;
	state->request_port = pg_batch_request_port_create(estate->es_query_cxt,
		pg_batch_limit_api, node->ss.ss_ScanTupleSlot, source_attnums,
		request_columns);
	pfree(source_attnums);
	pg_batch_request_port_set_request(state->request_port, NULL,
		project_columns, false);
	bms_free(project_columns);
	state->request = pg_batch_request_port_request(state->request_port);
	state->input = pg_batch_input_create(estate->es_query_cxt,
		pg_batch_limit_api, state->child, producer_name);
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

	if (state->offset_expr == NULL)
		state->offset_remaining = 0;
	else
	{
		value = ExecEvalExprSwitchContext(state->offset_expr, econtext, &isnull);
		state->offset_remaining = isnull ? 0 : DatumGetInt64(value);
		if (state->offset_remaining < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ROW_COUNT_IN_RESULT_OFFSET_CLAUSE),
					 errmsg("OFFSET must not be negative")));
	}

	if (state->count_expr == NULL)
	{
		state->count_remaining = 0;
		state->no_count = true;
	}
	else
	{
		value = ExecEvalExprSwitchContext(state->count_expr, econtext, &isnull);
		state->count_remaining = isnull ? 0 : DatumGetInt64(value);
		state->no_count = isnull;
		if (state->count_remaining < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_ROW_COUNT_IN_LIMIT_CLAUSE),
					 errmsg("LIMIT must not be negative")));
	}
	state->window_complete = !state->no_count && state->count_remaining == 0;
	state->limits_ready = true;
	if (state->no_count ||
		state->count_remaining > PG_INT64_MAX - state->offset_remaining)
		ExecSetTupleBound(-1, state->child);
	else
		ExecSetTupleBound(state->offset_remaining + state->count_remaining,
			state->child);
}

static void
forward_request(BatchLimitState *state)
{
	/* Limit consumes complete input batches even for a scalar parent. */
	pg_batch_input_forward_request(state->input, state->request, true);
	state->request_forwarded = true;
}

static int
trim_batch(BatchLimitState *state, PgBatch *batch)
{
	int			row = -1;

	while ((row = pg_batch_next_selected(batch, row)) >= 0)
	{
		if (state->offset_remaining > 0)
		{
			pg_batch_clear_row(batch, row);
			state->offset_remaining--;
			continue;
		}
		if (!state->no_count && state->count_remaining == 0)
		{
			pg_batch_clear_row(batch, row);
			continue;
		}
		state->output_rows++;
		if (!state->no_count)
			state->count_remaining--;
	}
	state->window_complete = !state->no_count && state->count_remaining == 0;
	return pg_batch_row_count(batch);
}

static bool
fetch_batch(BatchLimitState *state)
{
	for (;;)
	{
		PgBatchInputBatch input;
		int			rows;

		if (state->window_complete)
			return false;
		if (!pg_batch_input_next(state->input, &input))
			return false;
		state->input_batches++;
		state->input_rows += pg_batch_row_count(input.batch);
		rows = trim_batch(state, input.batch);
		if (rows == 0)
		{
			pg_batch_input_finish(state->input);
			continue;
		}
		/* Scalar output will read only projection columns of surviving rows. */
		if (!state->request->return_batch)
			pg_batch_materialize_columns(input.batch,
				state->request->project_columns, input.batch->selection,
				PG_BATCH_COLUMN_PROJECT);
		state->active_slot = input.slot;
		state->active_batch = input.batch;
		state->next_row = pg_batch_next_selected(input.batch, -1);
		pg_batch_input_select_row(state->input, state->next_row);
		return true;
	}
}

static TupleTableSlot *
empty_result(BatchLimitState *state)
{
	return ExecClearTuple(state->css.ss.ss_ScanTupleSlot);
}

static TupleTableSlot *
exec_batch(BatchLimitState *state)
{
	PlanState  *planstate = &state->css.ss.ps;
	int			rows;

	if (state->active_batch != NULL)
	{
		if (!pg_batch_input_batch_finished(state->input))
			elog(ERROR, "pg_batch_limit parent requested a new batch too early");
		state->active_batch = NULL;
		state->active_slot = NULL;
	}
	if (!fetch_batch(state))
		return empty_result(state);
	rows = pg_batch_row_count(state->active_batch);
	if (planstate->instrument != NULL)
		planstate->instrument->tuplecount += rows - 1;
	return state->active_slot;
}

static TupleTableSlot *
exec_row(BatchLimitState *state)
{
	for (;;)
	{
		int			row;

		if (state->active_batch == NULL)
		{
			if (!fetch_batch(state))
				return empty_result(state);
		}
		if (state->next_row < 0)
		{
			pg_batch_input_finish(state->input);
			state->active_batch = NULL;
			state->active_slot = NULL;
			continue;
		}
		row = state->next_row;
		state->next_row = pg_batch_next_selected(state->active_batch,
			row);
		return pg_batch_input_copy_row(state->input, row,
			state->css.ss.ss_ScanTupleSlot, state->batch_columns);
	}
}

static TupleTableSlot *
limit_exec(CustomScanState *node)
{
	BatchLimitState *state = (BatchLimitState *) node;

	if (!state->limits_ready)
		compute_limits(state);
	if (!state->request_forwarded)
		forward_request(state);
	if (state->request->return_batch)
		return exec_batch(state);
	return exec_row(state);
}

static void
limit_end(CustomScanState *node)
{
	BatchLimitState *state = (BatchLimitState *) node;

	ExecEndNode(state->child);
}

static void
limit_rescan(CustomScanState *node)
{
	BatchLimitState *state = (BatchLimitState *) node;

	ExecReScan(state->child);
	pg_batch_input_rescan(state->input);
	state->limits_ready = false;
	state->window_complete = false;
	state->active_slot = NULL;
	state->active_batch = NULL;
	state->next_row = -1;
	state->input_batches = 0;
	state->input_rows = 0;
	state->output_rows = 0;
}

static void
limit_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	BatchLimitState *state = (BatchLimitState *) node;

	if (es->analyze)
	{
		ExplainPropertyInteger("Input Batches", NULL,
			state->input_batches, es);
		ExplainPropertyInteger("Input Rows", NULL, state->input_rows, es);
		ExplainPropertyInteger("Output Rows", NULL, state->output_rows, es);
	}
}
