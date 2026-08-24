#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"

#include "pg_batch.h"

typedef struct PgBatchFilterState
{
	CustomScanState css;
	PlanState  *child;
	PgBatchSlot *active_batch;
	int			next_row;
	bool		request_forwarded;
	uint64		input_batches;
	uint64		input_rows;
	uint64		output_rows;
} PgBatchFilterState;

static const CustomExecMethods pg_batch_filter_exec_methods;

Node *
pg_batch_create_filter_state(CustomScan *cscan)
{
	PgBatchFilterState *state = palloc0_object(PgBatchFilterState);

	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &pg_batch_filter_exec_methods;
	state->css.slotOps = &PgBatchSlotOps;
	return (Node *) state;
}

static void pg_batch_filter_begin(CustomScanState *node, EState *estate,
								  int eflags);
static TupleTableSlot *pg_batch_filter_exec(CustomScanState *node);
static void pg_batch_filter_end(CustomScanState *node);
static void pg_batch_filter_rescan(CustomScanState *node);
static void pg_batch_filter_explain(CustomScanState *node, List *ancestors,
									ExplainState *es);

static const CustomExecMethods pg_batch_filter_exec_methods = {
	.CustomName = "PgBatchFilterProject",
	.BeginCustomScan = pg_batch_filter_begin,
	.ExecCustomScan = pg_batch_filter_exec,
	.EndCustomScan = pg_batch_filter_end,
	.ReScanCustomScan = pg_batch_filter_rescan,
	.ExplainCustomScan = pg_batch_filter_explain,
};

static Bitmapset *
pg_batch_intlist_bitmap(List *items)
{
	Bitmapset  *result = NULL;
	ListCell   *lc;

	foreach(lc, items)
		result = bms_add_member(result, lfirst_int(lc));
	return result;
}

static void
pg_batch_filter_begin(CustomScanState *node, EState *estate, int eflags)
{
	PgBatchFilterState *state = (PgBatchFilterState *) node;
	CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
	PgBatchSlot *request_slot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);
	List	   *source_attnums = (List *) linitial(cscan->custom_private);
	int			nfilter = intVal(lsecond(cscan->custom_private));
	List	   *survivor_items = lthird(cscan->custom_private);
	Bitmapset  *filter_columns = NULL;
	Bitmapset  *survivor_columns = pg_batch_intlist_bitmap(survivor_items);

	/* This slot advertises the compact layout and the parent's request. */
	pg_batch_configure_slot(request_slot, NULL, source_attnums, nfilter);
	if (nfilter > 0)
		filter_columns = bms_add_range(filter_columns, 0, nfilter - 1);
	pg_batch_set_request(request_slot, filter_columns, survivor_columns, false);
	bms_free(filter_columns);
	bms_free(survivor_columns);

	pg_batch_init_children(node, estate, eflags);
	state->child = linitial(node->custom_ps);
}

static PgBatchSlot *
pg_batch_filter_child_slot(PgBatchFilterState *state)
{
	CustomScanState *child = castNode(CustomScanState, state->child);

	return pg_batch_slot_cast(child->ss.ss_ScanTupleSlot);
}

static bool
pg_batch_apply_residual_quals(PgBatchFilterState *state, PgBatchSlot *bslot)
{
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	uint64		rows = bslot->selected_rows;
	uint64		initial = rows;

	if (state->css.ss.ps.qual == NULL)
		return rows != 0;
	while (rows != 0)
	{
		int			row = pg_rightmost_one_pos64(rows);
		uint64		bit = UINT64CONST(1) << row;

		ResetExprContext(econtext);
		pg_batch_slot_select_row(bslot, row);
		econtext->ecxt_scantuple = &bslot->base;
		if (!ExecQual(state->css.ss.ps.qual, econtext))
			bslot->selected_rows &= ~bit;
		rows &= ~bit;
	}
	InstrCountFiltered1(&state->css.ss,
						pg_batch_row_count(initial) -
						pg_batch_row_count(bslot->selected_rows));
	return bslot->selected_rows != 0;
}

static void
pg_batch_forward_request(PgBatchFilterState *state)
{
	PgBatchSlot *request_slot =
		pg_batch_slot_cast(state->css.ss.ss_ScanTupleSlot);
	PgBatchSlot *child_slot = pg_batch_filter_child_slot(state);

	Assert(!state->request_forwarded);
	/* The parent has finalized request_slot->request before its first call. */
	pg_batch_set_request(child_slot, request_slot->request.filter_columns,
						 request_slot->request.survivor_columns,
						 request_slot->request.return_batch);
	state->request_forwarded = true;
}

static PgBatchSlot *
pg_batch_fetch_batch(PgBatchFilterState *state)
{
	PgBatchSlot *request_slot =
		pg_batch_slot_cast(state->css.ss.ss_ScanTupleSlot);

	for (;;)
	{
		TupleTableSlot *slot;
		PgBatchSlot *bslot;
		int			rows;

		slot = ExecProcNode(state->child);
		if (TupIsNull(slot))
			return NULL;
		bslot = pg_batch_slot_cast(slot);
		state->input_batches++;
		state->input_rows += pg_batch_row_count(bslot->selected_rows);
		if (!pg_batch_apply_residual_quals(state, bslot))
		{
			pg_batch_finish_batch(bslot);
			continue;
		}
		if (request_slot->request.return_batch)
			pg_batch_prepare_columns(bslot,
									 request_slot->request.survivor_columns,
									 bslot->selected_rows,
									 PG_BATCH_PROJECT_PHASE);
		else
			pg_batch_materialize_columns(bslot,
										 request_slot->request.survivor_columns,
										 bslot->selected_rows,
										 PG_BATCH_PROJECT_PHASE);
		rows = pg_batch_row_count(bslot->selected_rows);
		state->output_rows += rows;
		state->active_batch = bslot;
		state->next_row = pg_rightmost_one_pos64(bslot->selected_rows);
		return bslot;
	}
}

static TupleTableSlot *
pg_batch_filter_empty_result(PgBatchFilterState *state)
{
	PlanState  *planstate = &state->css.ss.ps;

	if (planstate->ps_ProjInfo != NULL)
		return ExecClearTuple(planstate->ps_ProjInfo->pi_state.resultslot);
	return ExecClearTuple(state->css.ss.ss_ScanTupleSlot);
}

static TupleTableSlot *
pg_batch_filter_exec_batch(PgBatchFilterState *state)
{
	PgBatchSlot *bslot = state->active_batch;
	PlanState  *planstate = &state->css.ss.ps;

	/*
	 * A batch-aware parent must finish the previous batch before asking
	 * again.
	 */
	if (bslot != NULL)
	{
		if (!bslot->batch_consumed)
			elog(ERROR, "pg_batch parent requested a new batch too early");
		state->active_batch = NULL;
	}

	bslot = pg_batch_fetch_batch(state);
	if (bslot == NULL)
		return pg_batch_filter_empty_result(state);

	pg_batch_slot_select_row(bslot,
							 pg_rightmost_one_pos64(bslot->selected_rows));
	if (planstate->instrument != NULL)
		planstate->instrument->tuplecount +=
			pg_batch_row_count(bslot->selected_rows) - 1;
	return &bslot->base;
}

static TupleTableSlot *
pg_batch_filter_exec_row(PgBatchFilterState *state)
{
	PlanState  *planstate = &state->css.ss.ps;
	ExprContext *econtext = planstate->ps_ExprContext;

	for (;;)
	{
		PgBatchSlot *bslot = state->active_batch;

		if (bslot == NULL)
		{
			bslot = pg_batch_fetch_batch(state);
			if (bslot == NULL)
				return pg_batch_filter_empty_result(state);
		}

		while (state->next_row < bslot->nrows &&
			   (bslot->selected_rows &
				(UINT64CONST(1) << state->next_row)) == 0)
			state->next_row++;
		if (state->next_row >= bslot->nrows)
		{
			pg_batch_finish_batch(bslot);
			state->active_batch = NULL;
			continue;
		}

		pg_batch_slot_select_row(bslot, state->next_row++);
		if (planstate->ps_ProjInfo == NULL)
			return &bslot->base;
		ResetExprContext(econtext);
		econtext->ecxt_scantuple = &bslot->base;
		return ExecProject(planstate->ps_ProjInfo);
	}
}

static TupleTableSlot *
pg_batch_filter_exec(CustomScanState *node)
{
	PgBatchFilterState *state = (PgBatchFilterState *) node;
	PgBatchSlot *request_slot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);

	if (!state->request_forwarded)
		pg_batch_forward_request(state);
	if (request_slot->request.return_batch)
		return pg_batch_filter_exec_batch(state);
	return pg_batch_filter_exec_row(state);
}

static void
pg_batch_filter_end(CustomScanState *node)
{
	pg_batch_end_children(node);
}

static void
pg_batch_filter_rescan(CustomScanState *node)
{
	PgBatchFilterState *state = (PgBatchFilterState *) node;

	state->active_batch = NULL;
	state->next_row = 0;
	pg_batch_rescan_children(node);
}

static void
pg_batch_filter_explain(CustomScanState *node, List *ancestors,
						ExplainState *es)
{
	PgBatchFilterState *state = (PgBatchFilterState *) node;

	if (es->analyze)
	{
		ExplainPropertyInteger("Input Batches", NULL, state->input_batches, es);
		ExplainPropertyInteger("Input Rows", NULL, state->input_rows, es);
		ExplainPropertyInteger("Output Rows", NULL, state->output_rows, es);
	}
}
