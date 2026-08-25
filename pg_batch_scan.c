#include "postgres.h"

#include "access/heapam.h"
#include "access/tableam.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "fmgr.h"
#include "nodes/tidbitmap.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/fmgroids.h"
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
	PgBatchSourceOp source_op;
	bool		source_exact;
} PgBatchQualState;

typedef struct PgBatchScanState
{
	CustomScanState css;
	TableScanDesc scan;
	TupleTableSlot *heap_slot;
	PlanState  *bitmap_plan;
	TIDBitmap *tbm;
	MemoryContext operator_context;
	PgBatchQualState *quals;
	PgBatchSourceQual *source_quals;
	int			nquals;
	int			next_page_row;
	PgBatchHeapScanMode heap_scan_mode;
	bool		page_active;
	bool		first_batch_on_page;
	bool		bitmap_initialized;
	bool		compressed_source;
	bool		through_tableam;
	bool		source_request_ready;
	bool		done;
	uint64		batches;
	uint64		source_rows;
	uint64		lossy_pages;
	uint64		exact_pages;
} PgBatchScanState;

static const CustomExecMethods pg_batch_scan_exec_methods;

static PgBatchSourceOp
pg_batch_source_op(Oid funcid, uint8 var_argno)
{
	PgBatchSourceOp op;

	switch (funcid)
	{
		case F_INT4EQ:
			op = PG_BATCH_SOURCE_EQ;
			break;
		case F_INT4NE:
			op = PG_BATCH_SOURCE_NE;
			break;
		case F_INT4LT:
			op = PG_BATCH_SOURCE_LT;
			break;
		case F_INT4LE:
			op = PG_BATCH_SOURCE_LE;
			break;
		case F_INT4GT:
			op = PG_BATCH_SOURCE_GT;
			break;
		case F_INT4GE:
			op = PG_BATCH_SOURCE_GE;
			break;
		default:
			return PG_BATCH_SOURCE_UNSUPPORTED;
	}
	if (var_argno == 1)
	{
		if (op == PG_BATCH_SOURCE_LT)
			op = PG_BATCH_SOURCE_GT;
		else if (op == PG_BATCH_SOURCE_LE)
			op = PG_BATCH_SOURCE_GE;
		else if (op == PG_BATCH_SOURCE_GT)
			op = PG_BATCH_SOURCE_LT;
		else if (op == PG_BATCH_SOURCE_GE)
			op = PG_BATCH_SOURCE_LE;
	}
	return op;
}

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
	qual->source_op = pg_batch_source_op(op->opfuncid, var_argno);
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
	PgBatchHeapScanMode heap_scan_mode = intVal(lthird(cscan->custom_private));
	int			i = 0;

	if (eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_batch supports only forward scans")));
	pg_batch_configure_slot(bslot,
							RelationGetDescr(node->ss.ss_currentRelation),
							attnums, nfilter);
	state->heap_scan_mode = heap_scan_mode;
	/* A bitmap path is costed for and must continue to read the heap. */
	state->compressed_source = heap_scan_mode == PG_BATCH_HEAP_SEQ &&
		pg_batch_compressed_available(node->ss.ss_currentRelation);
	state->through_tableam = state->compressed_source &&
		pg_batch_compressed_via_tableam &&
		pg_batch_relation_uses_tableam(node->ss.ss_currentRelation);
	if (heap_scan_mode == PG_BATCH_HEAP_BITMAP)
	{
		uint32		flags = SO_NONE;

		pg_batch_init_children(node, estate, eflags);
		state->bitmap_plan = linitial(node->custom_ps);
		if (ScanRelIsReadOnly(&node->ss))
			flags |= SO_HINT_REL_READ_ONLY;
		if (estate->es_instrument & INSTRUMENT_IO)
			flags |= SO_SCAN_INSTRUMENT;
		state->scan = table_beginscan_bm(node->ss.ss_currentRelation,
										 estate->es_snapshot, 0, NULL, flags);
	}
	else if (!state->compressed_source || state->through_tableam)
		state->scan = table_beginscan(node->ss.ss_currentRelation,
								  estate->es_snapshot, 0, NULL, 0);
	if (!state->compressed_source)
	{
		state->heap_slot = ExecInitExtraTupleSlot(estate,
												  RelationGetDescr(node->ss.ss_currentRelation),
												  &TTSOpsBufferHeapTuple);
	}
	state->nquals = list_length(cscan->custom_exprs);
	state->quals = palloc0_array(PgBatchQualState, state->nquals);
	state->source_quals = palloc0_array(PgBatchSourceQual, state->nquals);
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

static void
pg_batch_bitmap_begin(PgBatchScanState *state)
{
	TBMIterator iterator;

	Assert(state->heap_scan_mode == PG_BATCH_HEAP_BITMAP);
	Assert(!state->bitmap_initialized);
	state->tbm = (TIDBitmap *) MultiExecProcNode(state->bitmap_plan);
	if (state->tbm == NULL || !IsA(state->tbm, TIDBitmap))
		elog(ERROR, "pg_batch bitmap child returned an invalid result");
	iterator = tbm_begin_iterate(state->tbm,
								 state->css.ss.ps.state->es_query_dsa,
								 InvalidDsaPointer);
	state->scan->st.rs_tbmiterator = iterator;
	state->bitmap_initialized = true;
}

static bool
pg_batch_heap_next_page(PgBatchScanState *state)
{
	HeapScanDesc hscan = (HeapScanDesc) state->scan;
	bool		recheck;
	bool		found;

	if (state->page_active)
	{
		if (state->heap_scan_mode == PG_BATCH_HEAP_BITMAP)
			hscan->rs_cindex = hscan->rs_ntuples;
		else
			hscan->rs_cindex = hscan->rs_ntuples - 1;
	}
	ExecClearTuple(state->heap_slot);
	if (state->heap_scan_mode == PG_BATCH_HEAP_BITMAP)
	{
		if (!state->bitmap_initialized)
			pg_batch_bitmap_begin(state);
		/* PgBatchScan rechecks every restriction, including exact pages. */
		found = table_scan_bitmap_next_tuple(state->scan, state->heap_slot,
										 &recheck,
										 &state->lossy_pages,
										 &state->exact_pages);
	}
	else
		found = heap_getnextslot(state->scan, ForwardScanDirection,
								 state->heap_slot);
	if (!found)
		return false;

	state->page_active = true;
	state->first_batch_on_page = true;
	state->next_page_row = 0;
	ExecClearTuple(state->heap_slot);
	return true;
}

static void
pg_batch_scan_prepare_source_request(PgBatchScanState *state,
								 PgBatchSlot *bslot)
{
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;

	ResetExprContext(econtext);
	for (int q = 0; q < state->nquals; q++)
	{
		PgBatchQualState *qual = &state->quals[q];
		PgBatchSourceQual *source = &state->source_quals[q];

		source->column = qual->column;
		source->op = qual->source_op;
		if (qual->other_expr != NULL)
			source->scalar.value = ExecEvalExpr(qual->other_expr, econtext,
											 &source->scalar.isnull);
		else
			source->scalar = qual->constant;
		qual->source_exact = state->compressed_source &&
			pg_batch_compressed_scan_mode == PG_BATCH_COMPRESSED_FILTER &&
			qual->source_op != PG_BATCH_SOURCE_UNSUPPORTED;
	}
	pg_batch_set_source_request(bslot, state->source_quals, state->nquals,
		(PgBatchCompressedScanMode) pg_batch_compressed_scan_mode);
	state->source_request_ready = true;
}

static bool
pg_batch_scan_next_batch(PgBatchScanState *state, PgBatchSlot *bslot)
{
	HeapScanDesc hscan;

	if (state->compressed_source)
	{
		bool		found;

		if (state->through_tableam)
			found = table_scan_getnextslot(state->scan, ForwardScanDirection,
									   &bslot->base);
		else
			found = pg_batch_compressed_scan_next(bslot,
											 state->css.ss.ss_currentRelation);
		if (!found)
		{
			state->done = true;
			return false;
		}
		state->batches++;
		state->source_rows += bslot->nrows;
		return true;
	}

	hscan = (HeapScanDesc) state->scan;

	for (;;)
	{
		int			nrows;

		if (!state->page_active ||
			state->next_page_row >= (int) hscan->rs_ntuples)
		{
			/*
			 * A core heap callback prunes the page and performs MVCC checks.
			 * For bitmap scans it also follows the TIDs selected by BRIN. We
			 * then publish batches over rs_vistuples while the scan and
			 * PgBatchSlot both keep the buffer pinned.
			 */
			if (!pg_batch_heap_next_page(state))
			{
				state->done = true;
				return false;
			}
			if (hscan->rs_ntuples == 0)
				continue;
		}

		nrows = Min(PG_BATCH_SIZE,
					(int) hscan->rs_ntuples - state->next_page_row);
		/* The core callback counted the first row of the first batch. */
		for (int i = state->first_batch_on_page ? 1 : 0; i < nrows; i++)
		{
			if (state->heap_scan_mode == PG_BATCH_HEAP_BITMAP)
				pgstat_count_heap_fetch(state->css.ss.ss_currentRelation);
			else
				pgstat_count_heap_getnext(state->css.ss.ss_currentRelation);
		}
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
	uint64		initial = pg_batch_nrows_mask(bslot->nrows);

	pg_batch_prepare_columns(bslot, bslot->request.filter_columns,
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

		/* The compressed source has already applied this exact condition. */
		if (qual->source_exact)
			continue;

		if (qual->other_expr != NULL)
			other->value = ExecEvalExpr(qual->other_expr, econtext,
										&other->isnull);
		else
			*other = qual->constant;
		if (other->isnull)
			bslot->selected_rows = 0;
		else
		{
			track_function =
				pgstat_track_functions > qual->fcinfo->flinfo->fn_stats;
			oldoperator = MemoryContextSwitchTo(state->operator_context);

			/*
			 * Keep the planner's qual order, but run each qual over the
			 * batch.
			 */
			rows = bslot->selected_rows;
			if (likely(bslot->format_ops == NULL))
			{
				PgBatchColumn *column = &bslot->columns[qual->column];

				while (rows != 0)
				{
					int			row = pg_rightmost_one_pos64(rows);
					uint64		bit = UINT64CONST(1) << row;

					Assert((column->valid_rows & bit) != 0);
					if (column->isnull[row] ||
						!pg_batch_eval_qual(qual,
											column->values[row],
											track_function))
						bslot->selected_rows &= ~bit;
					rows &= ~bit;
				}
			}
			else
			{
				PgBatchArrowView column =
					pg_batch_get_arrow_column(bslot, qual->column);
				const struct ArrowArray *array = column.array;
				const int32 *values = array->buffers[1];

				Assert(strcmp(column.schema->format, "i") == 0);
				while (rows != 0)
				{
					int			row = pg_rightmost_one_pos64(rows);
					uint64		bit = UINT64CONST(1) << row;

					if (!pg_batch_arrow_row_is_valid(array, row) ||
						!pg_batch_eval_qual(qual,
											Int32GetDatum(values[array->offset + row]),
											track_function))
						bslot->selected_rows &= ~bit;
					rows &= ~bit;
				}
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
	if (!state->source_request_ready)
		pg_batch_scan_prepare_source_request(state, bslot);
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
pg_batch_bitmap_end(PgBatchScanState *state)
{
	if (!state->bitmap_initialized)
		return;
	if (!tbm_exhausted(&state->scan->st.rs_tbmiterator))
		tbm_end_iterate(&state->scan->st.rs_tbmiterator);
	tbm_free(state->tbm);
	state->tbm = NULL;
	state->bitmap_initialized = false;
}

static void
pg_batch_scan_end(CustomScanState *node)
{
	PgBatchScanState *state = (PgBatchScanState *) node;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	pg_batch_bitmap_end(state);
	if (state->scan != NULL)
		table_endscan(state->scan);
	pg_batch_end_children(node);
	pg_batch_compressed_scan_end(
		pg_batch_slot_cast(node->ss.ss_ScanTupleSlot));
}

static void
pg_batch_scan_rescan(CustomScanState *node)
{
	PgBatchScanState *state = (PgBatchScanState *) node;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	pg_batch_bitmap_end(state);
	if (state->scan != NULL)
		table_rescan(state->scan, NULL);
	if (state->bitmap_plan != NULL && state->bitmap_plan->chgParam == NULL)
		ExecReScan(state->bitmap_plan);
	pg_batch_compressed_scan_rescan(
		pg_batch_slot_cast(node->ss.ss_ScanTupleSlot));
	state->page_active = false;
	state->first_batch_on_page = false;
	state->next_page_row = 0;
	state->source_request_ready = false;
	state->done = false;
}

static void
pg_batch_scan_explain(CustomScanState *node, List *ancestors,
					  ExplainState *es)
{
	PgBatchScanState *state = (PgBatchScanState *) node;
	PgBatchSlot *bslot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);
	const PgBatchCompressedStats *compressed_stats =
		pg_batch_compressed_scan_stats(bslot);

	ExplainPropertyInteger("Batch Size", NULL, PG_BATCH_SIZE, es);
	ExplainPropertyText("Batch Source",
						state->compressed_source ? "Compressed Arrow" :
						state->heap_scan_mode == PG_BATCH_HEAP_BITMAP ?
						"Heap Bitmap" : "Heap", es);
	if (state->compressed_source)
	{
		const char *mode;

		if (pg_batch_compressed_scan_mode == PG_BATCH_COMPRESSED_FILTER)
			mode = "filter";
		else if (pg_batch_compressed_scan_mode == PG_BATCH_COMPRESSED_PRUNE)
			mode = "prune";
		else
			mode = "batch";
		ExplainPropertyText("Compressed Path",
							state->through_tableam ? "Table AM" : "Direct", es);
		ExplainPropertyText("Compressed Scan Mode", mode, es);
	}
	if (es->analyze)
	{
		ExplainPropertyInteger("Batches", NULL, state->batches, es);
		ExplainPropertyInteger("Source Rows", NULL, state->source_rows, es);
		ExplainPropertyInteger("Filter Datums", NULL, bslot->filter_datums, es);
		ExplainPropertyInteger("Projection Datums", NULL,
							   bslot->project_datums, es);
		ExplainPropertyInteger("Restarted Projection Datums", NULL,
							   bslot->restarted_project_datums, es);
		if (state->heap_scan_mode == PG_BATCH_HEAP_BITMAP)
		{
			ExplainPropertyInteger("Exact Heap Blocks", NULL,
								   state->exact_pages, es);
			ExplainPropertyInteger("Lossy Heap Blocks", NULL,
								   state->lossy_pages, es);
		}
		if (state->compressed_source)
		{
			if (compressed_stats != NULL)
			{
				ExplainPropertyInteger("Storage Groups Examined", NULL,
								   compressed_stats->groups_examined, es);
				ExplainPropertyInteger("Groups Skipped by Min/Max", NULL,
								   compressed_stats->groups_skipped_minmax, es);
				ExplainPropertyInteger("Groups Skipped by Membership", NULL,
								   compressed_stats->groups_skipped_membership, es);
				ExplainPropertyInteger("Rows Removed by Source Filter", NULL,
								   compressed_stats->rows_removed_by_source_filter, es);
				ExplainPropertyInteger("Encoded Bytes Touched", NULL,
								   compressed_stats->encoded_bytes_touched, es);
			}
			ExplainPropertyInteger("Arrow Filter Columns", NULL,
								   bslot->arrow_filter_columns, es);
			ExplainPropertyInteger("Arrow Projection Columns", NULL,
								   bslot->arrow_project_columns, es);
			ExplainPropertyInteger("Arrow Decoded Values", NULL,
								   bslot->arrow_decoded_values, es);
		}
	}
}
