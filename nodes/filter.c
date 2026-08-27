#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"

#include "internal.h"

typedef struct BatchFilterState
{
	CustomScanState css;
	PlanState  *child;
	const PgBatchBridgeRequest *request;
	TupleTableSlot *active_slot;
	PgBatchBridgeBatch *active_batch;
	int			next_row;
	bool		request_forwarded;
	uint64		input_batches;
	uint64		input_rows;
	uint64		output_rows;
} BatchFilterState;

static const CustomExecMethods pg_batch_filter_exec_methods;

Node *
pg_batch_create_filter_state(CustomScan *cscan)
{
	BatchFilterState *state = palloc0_object(BatchFilterState);

	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &pg_batch_filter_exec_methods;
	state->css.slotOps = &PgBatchSlotOps;
	return (Node *) state;
}

static void filter_begin(CustomScanState *node, EState *estate,
						 int eflags);
static TupleTableSlot *filter_exec(CustomScanState *node);
static void filter_end(CustomScanState *node);
static void filter_rescan(CustomScanState *node);
static void filter_explain(CustomScanState *node, List *ancestors,
						   ExplainState *es);

static const CustomExecMethods pg_batch_filter_exec_methods = {
	.CustomName = "PgBatchFilterProject",
	.BeginCustomScan = filter_begin,
	.ExecCustomScan = filter_exec,
	.EndCustomScan = filter_end,
	.ReScanCustomScan = filter_rescan,
	.ExplainCustomScan = filter_explain,
};

static Bitmapset *
intlist_to_bitmap(List *items)
{
	Bitmapset  *result = NULL;
	ListCell   *lc;

	foreach(lc, items)
		result = bms_add_member(result, lfirst_int(lc));
	return result;
}

static void
filter_begin(CustomScanState *node, EState *estate, int eflags)
{
	BatchFilterState *state = (BatchFilterState *) node;
	CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
	PgBatchSlot *request_slot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);
	List	   *source_attnums = (List *) linitial(cscan->custom_private);
	int			nfilter = intVal(lsecond(cscan->custom_private));
	List	   *survivor_items = lthird(cscan->custom_private);
	Bitmapset  *filter_columns = NULL;
	Bitmapset  *survivor_columns = intlist_to_bitmap(survivor_items);

	/* This slot advertises the compact layout and the parent's request. */
	pg_batch_configure_slot(request_slot, NULL, source_attnums, nfilter);
	state->request = pg_batch_bridge->get_request(request_slot->binding);
	if (nfilter > 0)
		filter_columns = bms_add_range(filter_columns, 0, nfilter - 1);
	pg_batch_set_request(&request_slot->base, filter_columns, survivor_columns,
						 false);
	bms_free(filter_columns);
	bms_free(survivor_columns);

	pg_batch_init_children(node, estate, eflags);
	state->child = linitial(node->custom_ps);
}

static TupleTableSlot *
filter_child_slot(BatchFilterState *state)
{
	CustomScanState *child = castNode(CustomScanState, state->child);

	return child->ss.ss_ScanTupleSlot;
}

static int
apply_residual_quals(BatchFilterState *state,
					 TupleTableSlot *slot,
					 PgBatchBridgeBatch *batch, int initial)
{
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	int			row = -1;
	int			remaining;

	if (state->css.ss.ps.qual == NULL)
		return initial;
	while ((row = pg_batch_bridge_next_selected(batch, row)) >= 0)
	{
		ResetExprContext(econtext);
		pg_batch_select_row(slot, row);
		econtext->ecxt_scantuple = slot;
		if (!ExecQual(state->css.ss.ps.qual, econtext))
			pg_batch_bridge_clear_row(batch, row);
	}
	remaining = pg_batch_row_count(batch);
	InstrCountFiltered1(&state->css.ss, initial - remaining);
	return remaining;
}

static void
forward_request(BatchFilterState *state)
{
	TupleTableSlot *child_slot = filter_child_slot(state);

	Assert(!state->request_forwarded);
	/* The parent finalizes this request before its first ExecProcNode(). */
	pg_batch_set_request(child_slot, state->request->filter_columns,
						 state->request->survivor_columns,
						 state->request->return_batch);
	state->request_forwarded = true;
}

static int
fetch_batch(BatchFilterState *state)
{
	for (;;)
	{
		TupleTableSlot *slot;
		PgBatchBridgeBatch *batch;
		int			rows;

		slot = ExecProcNode(state->child);
		if (TupIsNull(slot))
			return 0;
		batch = pg_batch_get_batch(slot);
		state->input_batches++;
		rows = pg_batch_row_count(batch);
		state->input_rows += rows;
		rows = apply_residual_quals(state, slot, batch, rows);
		if (rows == 0)
		{
			pg_batch_finish_batch(slot);
			continue;
		}
		/* A batch-aware parent prepares survivor columns when it uses them. */
		if (!state->request->return_batch)
			pg_batch_materialize_columns(batch,
									 state->request->survivor_columns,
										 batch->selection,
										 PG_BATCH_PROJECT_PHASE);
		state->output_rows += rows;
		state->active_slot = slot;
		state->active_batch = batch;
		state->next_row = pg_batch_bridge_next_selected(batch, -1);
		return rows;
	}
}

static TupleTableSlot *
empty_result(BatchFilterState *state)
{
	PlanState  *planstate = &state->css.ss.ps;

	if (planstate->ps_ProjInfo != NULL)
		return ExecClearTuple(planstate->ps_ProjInfo->pi_state.resultslot);
	return ExecClearTuple(state->css.ss.ss_ScanTupleSlot);
}

static TupleTableSlot *
exec_batch(BatchFilterState *state)
{
	PgBatchBridgeBatch *batch = state->active_batch;
	PlanState  *planstate = &state->css.ss.ps;
	int			rows;

	/*
	 * A batch-aware parent must finish the previous batch before asking
	 * again.
	 */
	if (batch != NULL)
	{
		if (!batch->consumed)
			elog(ERROR, "pg_batch parent requested a new batch too early");
		state->active_batch = NULL;
		state->active_slot = NULL;
	}

	rows = fetch_batch(state);
	if (rows == 0)
		return empty_result(state);
	pg_batch_select_row(state->active_slot, state->next_row);
	if (planstate->instrument != NULL)
		planstate->instrument->tuplecount += rows - 1;
	return state->active_slot;
}

static TupleTableSlot *
exec_row(BatchFilterState *state)
{
	PlanState  *planstate = &state->css.ss.ps;
	ExprContext *econtext = planstate->ps_ExprContext;

	for (;;)
	{
		PgBatchBridgeBatch *batch = state->active_batch;

		if (batch == NULL)
		{
			if (fetch_batch(state) == 0)
				return empty_result(state);
			batch = state->active_batch;
		}

		if (state->next_row < 0)
		{
			pg_batch_finish_batch(state->active_slot);
			state->active_batch = NULL;
			state->active_slot = NULL;
			continue;
		}

		pg_batch_select_row(state->active_slot, state->next_row);
		state->next_row =
			pg_batch_bridge_next_selected(batch, state->next_row);
		if (planstate->ps_ProjInfo == NULL)
			return state->active_slot;
		ResetExprContext(econtext);
		econtext->ecxt_scantuple = state->active_slot;
		return ExecProject(planstate->ps_ProjInfo);
	}
}

static TupleTableSlot *
filter_exec(CustomScanState *node)
{
	BatchFilterState *state = (BatchFilterState *) node;

	if (!state->request_forwarded)
		forward_request(state);
	if (state->request->return_batch)
		return exec_batch(state);
	return exec_row(state);
}

static void
filter_end(CustomScanState *node)
{
	pg_batch_end_children(node);
}

static void
filter_rescan(CustomScanState *node)
{
	BatchFilterState *state = (BatchFilterState *) node;

	state->active_batch = NULL;
	state->active_slot = NULL;
	state->next_row = 0;
	pg_batch_rescan_children(node);
}

static void
filter_explain(CustomScanState *node, List *ancestors,
			   ExplainState *es)
{
	BatchFilterState *state = (BatchFilterState *) node;

	if (es->analyze)
	{
		ExplainPropertyInteger("Input Batches", NULL, state->input_batches, es);
		ExplainPropertyInteger("Input Rows", NULL, state->input_rows, es);
		ExplainPropertyInteger("Output Rows", NULL, state->output_rows, es);
	}
}
