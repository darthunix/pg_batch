#include "postgres.h"

#include "access/heapam.h"
#include "access/tableam.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "fmgr.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "pg_batch.h"

typedef struct PgBatchQualState
{
	FmgrInfo	func;
	FunctionCallInfo fcinfo;
	ExprState  *other_expr;
	NullableDatum constant;
	int			column;
	uint8		var_argno;
} PgBatchQualState;

typedef struct PgBatchScanState
{
	CustomScanState css;
	TableScanDesc scan;
	TupleTableSlot *heap_slot;
	MemoryContext operator_context;
	PgBatchQualState *quals;
	int			nquals;
	int			next_page_row;
	bool		page_active;
	bool		first_batch_on_page;
	bool		done;
	uint64		batches;
	uint64		source_rows;
} PgBatchScanState;

static const CustomExecMethods pg_batch_scan_exec_methods;

static void
pg_batch_init_qual(PgBatchQualState *qual, OpExpr *op, uint8 var_argno,
				   PlanState *parent)
{
	Node	   *varnode = pg_batch_strip_relabel(list_nth(op->args, var_argno));
	Node	   *other = pg_batch_strip_relabel(list_nth(op->args, 1 - var_argno));
	Var		   *var = castNode(Var, varnode);

	fmgr_info(op->opfuncid, &qual->func);
	fmgr_info_set_expr((Node *) op, &qual->func);
	qual->fcinfo = palloc0(SizeForFunctionCallInfo(2));
	InitFunctionCallInfoData(*qual->fcinfo, &qual->func, 2,
							 op->inputcollid, NULL, NULL);
	qual->column = var->varattno - 1;
	qual->var_argno = var_argno;
	if (IsA(other, Const))
	{
		qual->constant.value = castNode(Const, other)->constvalue;
		qual->constant.isnull = castNode(Const, other)->constisnull;
	}
	else
		qual->other_expr = ExecInitExpr((Expr *) other, parent);
}

Node *
pg_batch_create_scan_state(CustomScan *cscan)
{
	PgBatchScanState *state = palloc0_object(PgBatchScanState);

	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &pg_batch_scan_exec_methods;
	state->css.slotOps = &PgBatchSlotOps;
	return (Node *) state;
}

static void pg_batch_scan_begin(CustomScanState *node, EState *estate,
								int eflags);
static TupleTableSlot *pg_batch_scan_exec(CustomScanState *node);
static void pg_batch_scan_end(CustomScanState *node);
static void pg_batch_scan_rescan(CustomScanState *node);
static void pg_batch_scan_explain(CustomScanState *node, List *ancestors,
								  ExplainState *es);

static const CustomExecMethods pg_batch_scan_exec_methods = {
	.CustomName = "PgBatchScan",
	.BeginCustomScan = pg_batch_scan_begin,
	.ExecCustomScan = pg_batch_scan_exec,
	.EndCustomScan = pg_batch_scan_end,
	.ReScanCustomScan = pg_batch_scan_rescan,
	.ExplainCustomScan = pg_batch_scan_explain,
};

static void
pg_batch_scan_begin(CustomScanState *node, EState *estate, int eflags)
{
	PgBatchScanState *state = (PgBatchScanState *) node;
	CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
	PgBatchSlot *bslot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);
	List	   *attnums = (List *) linitial(cscan->custom_private);
	int			nfilter = intVal(lsecond(cscan->custom_private));
	int			i = 0;

	if (eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_batch supports only forward scans")));
	pg_batch_configure_slot(bslot,
							RelationGetDescr(node->ss.ss_currentRelation),
							attnums, nfilter);
	state->scan = table_beginscan(node->ss.ss_currentRelation,
								  estate->es_snapshot, 0, NULL, 0);
	state->heap_slot = ExecInitExtraTupleSlot(estate,
											  RelationGetDescr(node->ss.ss_currentRelation),
											  &TTSOpsBufferHeapTuple);
	state->nquals = list_length(cscan->custom_exprs);
	state->quals = palloc0_array(PgBatchQualState, state->nquals);
	foreach_ptr(OpExpr, op, cscan->custom_exprs)
	{
		uint8		var_argno = 0;

		Assert(pg_batch_match_qual((Node *) op, &var_argno) != NULL);
		pg_batch_init_qual(&state->quals[i++], op, var_argno, &node->ss.ps);
	}
	state->operator_context =
		AllocSetContextCreate(node->ss.ps.ps_ExprContext->ecxt_per_query_memory,
							  "pg_batch operator", ALLOCSET_START_SMALL_SIZES);
}

static bool
pg_batch_scan_next_batch(PgBatchScanState *state, PgBatchSlot *bslot)
{
	HeapScanDesc hscan = (HeapScanDesc) state->scan;

	for (;;)
	{
		int			nrows;

		if (!state->page_active ||
			state->next_page_row >= (int) hscan->rs_ntuples)
		{
			/*
			 * heap_getnextslot() makes core prune the page and perform MVCC
			 * checks. We then publish batches over its rs_vistuples array
			 * while the heap scan and PgBatchSlot both keep the buffer
			 * pinned.
			 */
			if (state->page_active)
				hscan->rs_cindex = hscan->rs_ntuples - 1;
			ExecClearTuple(state->heap_slot);
			if (!heap_getnextslot(state->scan, ForwardScanDirection,
								  state->heap_slot))
			{
				state->done = true;
				return false;
			}
			state->page_active = true;
			state->first_batch_on_page = true;
			state->next_page_row = 0;
			ExecClearTuple(state->heap_slot);
			if (hscan->rs_ntuples == 0)
				continue;
		}

		nrows = Min(PG_BATCH_SIZE,
					(int) hscan->rs_ntuples - state->next_page_row);
		/* heap_getnextslot() counted the first row of the first batch. */
		for (int i = state->first_batch_on_page ? 1 : 0; i < nrows; i++)
			pgstat_count_heap_getnext(state->css.ss.ss_currentRelation);
		state->first_batch_on_page = false;
		pg_batch_load_batch(bslot, hscan->rs_cbuf, hscan->rs_cblock,
							RelationGetRelid(state->css.ss.ss_currentRelation),
							hscan->rs_vistuples + state->next_page_row, nrows);
		state->next_page_row += nrows;
		state->batches++;
		state->source_rows += nrows;
		return true;
	}
}

static inline bool
pg_batch_eval_qual(PgBatchQualState *qual, Datum value, bool track_function)
{
	FunctionCallInfo fcinfo = qual->fcinfo;
	PgStat_FunctionCallUsage usage;
	Datum		result;

	fcinfo->args[qual->var_argno].value = value;
	fcinfo->args[qual->var_argno].isnull = false;
	fcinfo->isnull = false;
	if (track_function)
		pgstat_init_function_usage(fcinfo, &usage);
	result = FunctionCallInvoke(fcinfo);
	if (track_function)
		pgstat_end_function_usage(&usage, true);
	return !fcinfo->isnull && DatumGetBool(result);
}

static void
pg_batch_scan_filter(PgBatchScanState *state, PgBatchSlot *bslot)
{
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	MemoryContext oldcontext;
	uint64		initial = bslot->selected_rows;

	pg_batch_materialize_columns(bslot, bslot->request.filter_columns,
								 bslot->selected_rows, PG_BATCH_FILTER_PHASE);
	ResetExprContext(econtext);
	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	for (int q = 0; q < state->nquals && bslot->selected_rows != 0; q++)
	{
		PgBatchQualState *qual = &state->quals[q];
		NullableDatum *other = &qual->fcinfo->args[1 - qual->var_argno];
		MemoryContext oldoperator;
		bool		track_function;
		uint64		rows;

		if (qual->other_expr != NULL)
			other->value = ExecEvalExpr(qual->other_expr, econtext,
										&other->isnull);
		else
			*other = qual->constant;
		if (other->isnull)
			bslot->selected_rows = 0;
		else
		{
			PgBatchColumn *column = &bslot->columns[qual->column];

			track_function =
				pgstat_track_functions > qual->fcinfo->flinfo->fn_stats;
			oldoperator = MemoryContextSwitchTo(state->operator_context);

			/*
			 * Keep the planner's qual order, but run each qual over the
			 * batch.
			 */
			rows = bslot->selected_rows;
			while (rows != 0)
			{
				int			row = pg_rightmost_one_pos64(rows);
				uint64		bit = UINT64CONST(1) << row;

				if (column->isnull[row] ||
					!pg_batch_eval_qual(qual, column->values[row],
										track_function))
					bslot->selected_rows &= ~bit;
				rows &= ~bit;
			}
			MemoryContextSwitchTo(oldoperator);
		}
		if (qual->other_expr != NULL)
		{
			other->value = (Datum) 0;
			other->isnull = true;
		}
		MemoryContextReset(state->operator_context);
	}
	MemoryContextSwitchTo(oldcontext);
	InstrCountFiltered1(&state->css.ss, pg_batch_row_count(initial) -
						pg_batch_row_count(bslot->selected_rows));
}

static TupleTableSlot *
pg_batch_scan_exec(CustomScanState *node)
{
	PgBatchScanState *state = (PgBatchScanState *) node;
	PgBatchSlot *bslot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);

	if (!ScanDirectionIsForward(node->ss.ps.state->es_direction))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_batch supports only forward scans")));
	if (state->done)
		return ExecClearTuple(&bslot->base);
	if (bslot->nrows > 0)
	{
		if (!bslot->batch_consumed)
			elog(ERROR, "pg_batch consumer requested a new batch too early");
		ExecClearTuple(&bslot->base);
	}

	while (pg_batch_scan_next_batch(state, bslot))
	{
		int			rows;

		pg_batch_scan_filter(state, bslot);
		if (bslot->selected_rows == 0)
		{
			pg_batch_finish_batch(bslot);
			ExecClearTuple(&bslot->base);
			continue;
		}
		rows = pg_batch_row_count(bslot->selected_rows);
		pg_batch_slot_select_row(bslot,
								 pg_rightmost_one_pos64(bslot->selected_rows));
		if (node->ss.ps.instrument != NULL)
			node->ss.ps.instrument->tuplecount += rows - 1;
		return &bslot->base;
	}
	return ExecClearTuple(&bslot->base);
}

static void
pg_batch_scan_end(CustomScanState *node)
{
	PgBatchScanState *state = (PgBatchScanState *) node;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	if (state->scan != NULL)
		table_endscan(state->scan);
}

static void
pg_batch_scan_rescan(CustomScanState *node)
{
	PgBatchScanState *state = (PgBatchScanState *) node;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	table_rescan(state->scan, NULL);
	state->page_active = false;
	state->first_batch_on_page = false;
	state->next_page_row = 0;
	state->done = false;
}

static void
pg_batch_scan_explain(CustomScanState *node, List *ancestors,
					  ExplainState *es)
{
	PgBatchScanState *state = (PgBatchScanState *) node;
	PgBatchSlot *bslot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);

	ExplainPropertyInteger("Batch Size", NULL, PG_BATCH_SIZE, es);
	if (es->analyze)
	{
		ExplainPropertyInteger("Batches", NULL, state->batches, es);
		ExplainPropertyInteger("Source Rows", NULL, state->source_rows, es);
		ExplainPropertyInteger("Filter Datums", NULL, bslot->filter_datums, es);
		ExplainPropertyInteger("Projection Datums", NULL,
							   bslot->project_datums, es);
		ExplainPropertyInteger("Restarted Projection Datums", NULL,
							   bslot->restarted_project_datums, es);
	}
}
