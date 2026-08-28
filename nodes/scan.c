#include "postgres.h"

#include "access/heapam.h"
#include "access/tableam.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "fmgr.h"
#include "nodes/nodeFuncs.h"
#include "nodes/tidbitmap.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "internal.h"

typedef struct BatchQualState
{
	FmgrInfo	func;
	FunctionCallInfo fcinfo;
	ExprState  *other_expr;
	NullableDatum constant;
	int			column;
	uint8		var_argno;
	bool		recheck_only;
	bool		has_int4_kernel;
	PgBatchInt4Op int4_op;
} BatchQualState;

typedef struct BatchScanState
{
	CustomScanState css;
	TableScanDesc scan;
	TupleTableSlot *heap_slot;
	PlanState  *bitmap_plan;
	TIDBitmap  *tbm;
	MemoryContext operator_context;
	Bitmapset  *exact_filter_columns;
	BatchQualState *quals;
	int			nquals;
	int			next_page_row;
	PgBatchHeapScanMode heap_scan_mode;
	bool		page_active;
	bool		page_recheck;
	bool		first_batch_on_page;
	bool		bitmap_initialized;
	const		PgBatchBridgeRequest *request;
	const		PgBatchBridgeProviderOps *provider;
	void	   *provider_state;
	bool		done;
	uint64		batches;
	uint64		source_rows;
	uint64		lossy_pages;
	uint64		exact_pages;
	uint64		exact_rechecks_skipped;
} BatchScanState;

static const CustomExecMethods pg_batch_scan_exec_methods;

static void
init_qual(BatchQualState *qual, OpExpr *op, uint8 var_argno,
		  bool recheck_only, PlanState *parent)
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
	qual->recheck_only = recheck_only;
	qual->has_int4_kernel =
		var->vartype == INT4OID && exprType(other) == INT4OID &&
		pg_batch_int4_compare_op(op->opfuncid, var_argno, &qual->int4_op);
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
	BatchScanState *state = palloc0_object(BatchScanState);

	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &pg_batch_scan_exec_methods;
	state->css.slotOps = &PgBatchSlotOps;
	return (Node *) state;
}

static void scan_begin(CustomScanState *node, EState *estate,
					   int eflags);
static TupleTableSlot *scan_exec(CustomScanState *node);
static void scan_end(CustomScanState *node);
static void scan_rescan(CustomScanState *node);
static void scan_explain(CustomScanState *node, List *ancestors,
						 ExplainState *es);

static const CustomExecMethods pg_batch_scan_exec_methods = {
	.CustomName = "PgBatchScan",
	.BeginCustomScan = scan_begin,
	.ExecCustomScan = scan_exec,
	.EndCustomScan = scan_end,
	.ReScanCustomScan = scan_rescan,
	.ExplainCustomScan = scan_explain,
};

static void
scan_begin(CustomScanState *node, EState *estate, int eflags)
{
	BatchScanState *state = (BatchScanState *) node;
	CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
	PgBatchSlot *bslot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);
	List	   *attnums = (List *) linitial(cscan->custom_private);
	int			nfilter = intVal(lsecond(cscan->custom_private));
	PgBatchHeapScanMode heap_scan_mode = intVal(lthird(cscan->custom_private));
	const char *provider_name = strVal(list_nth(cscan->custom_private, 3));
	Node	   *source_private = list_nth(cscan->custom_private, 4);
	int			nsource_exprs = intVal(list_nth(cscan->custom_private, 5));
	List	   *batch_recheck_flags =
		(List *) list_nth(cscan->custom_private, 6);
	List	   *exact_filter_items =
		(List *) list_nth(cscan->custom_private, 7);
	List	   *source_exprs = list_copy_head(cscan->custom_exprs,
											  nsource_exprs);
	List	   *local_quals = list_copy_tail(cscan->custom_exprs,
											 nsource_exprs);
	PgBatchBridgeBinding *binding;
	int			i = 0;

	if (eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_batch supports only forward scans")));
	pg_batch_configure_slot(bslot,
							RelationGetDescr(node->ss.ss_currentRelation),
							attnums, nfilter);
	binding = bslot->binding;
	state->request = pg_batch_bridge->get_request(binding);
	state->heap_scan_mode = heap_scan_mode;
	if (provider_name[0] != '\0')
	{
		PgBatchBridgeExecRequest request;

		state->provider = pg_batch_bridge->get_provider(provider_name);
		if (state->provider == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("pg_batch provider \"%s\" is not loaded",
							provider_name)));
		if (!state->provider->supports_relation(node->ss.ss_currentRelation))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("pg_batch provider \"%s\" no longer supports relation \"%s\"",
							provider_name,
							RelationGetRelationName(node->ss.ss_currentRelation))));
		MemSet(&request, 0, sizeof(request));
		request.relation = node->ss.ss_currentRelation;
		request.parent = &node->ss.ps;
		request.source_private = source_private;
		request.source_exprs = source_exprs;
		request.query_context = estate->es_query_cxt;
		request.slot_request = state->request;
		state->provider_state = state->provider->begin_scan(&request);
		pg_batch_bridge->set_provider(binding, provider_name,
									  state->provider_state);
	}
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
	else if (state->provider == NULL || state->provider->next_batch == NULL)
		state->scan = table_beginscan(node->ss.ss_currentRelation,
									  estate->es_snapshot, 0, NULL, 0);
	if (state->provider == NULL)
	{
		state->heap_slot = ExecInitExtraTupleSlot(estate,
												  RelationGetDescr(node->ss.ss_currentRelation),
												  &TTSOpsBufferHeapTuple);
	}
	state->nquals = list_length(local_quals);
	Assert(list_length(batch_recheck_flags) == state->nquals);
	state->quals = palloc0_array(BatchQualState, state->nquals);
	foreach_ptr(OpExpr, op, local_quals)
	{
		uint8		var_argno = 0;
		bool		recheck_only =
			list_nth_int(batch_recheck_flags, i) != 0;

		if (pg_batch_match_qual((Node *) op, &var_argno) == NULL)
			elog(ERROR, "pg_batch received an invalid batch condition");
		init_qual(&state->quals[i++], op, var_argno, recheck_only,
				  &node->ss.ps);
	}
	foreach_int(column, exact_filter_items)
		state->exact_filter_columns =
			bms_add_member(state->exact_filter_columns, column);
	state->operator_context =
		AllocSetContextCreate(node->ss.ps.ps_ExprContext->ecxt_per_query_memory,
							  "pg_batch operator", ALLOCSET_START_SMALL_SIZES);
}

static void
bitmap_begin(BatchScanState *state)
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
heap_next_page(BatchScanState *state)
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
			bitmap_begin(state);
		/* Preserve whether the table AM requires index-qual rechecks. */
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
	state->page_recheck =
		state->heap_scan_mode == PG_BATCH_HEAP_BITMAP && recheck;
	state->first_batch_on_page = true;
	state->next_page_row = 0;
	ExecClearTuple(state->heap_slot);
	return true;
}

static bool
next_batch(BatchScanState *state, PgBatchSlot *bslot)
{
	HeapScanDesc hscan;

	if (state->provider != NULL)
	{
		bool		found;

		if (state->provider->next_batch != NULL)
		{
			PgBatchBridgeBatch *batch =
				state->provider->next_batch(state->provider_state);

			found = batch != NULL;
			if (found)
				pg_batch_bridge->publish_batch(bslot->binding, batch);
		}
		else
			found = table_scan_getnextslot(state->scan,
										   ForwardScanDirection,
										   &bslot->base);

		if (!found)
		{
			state->done = true;
			return false;
		}
		state->batches++;
		state->source_rows += pg_batch_get_batch(&bslot->base)->nrows;
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
			 * For bitmap scans it also follows the TIDs selected by any index
			 * AM or bitmap expression. We then publish batches over
			 * rs_vistuples while the scan and PgBatchSlot both keep the buffer
			 * pinned.
			 */
			if (!heap_next_page(state))
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
eval_qual(BatchQualState *qual, Datum value, bool track_function)
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

static int
filter_batch(BatchScanState *state, PgBatchSlot *bslot)
{
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	PgBatchBridgeBatch *batch = pg_batch_get_batch(&bslot->base);
	MemoryContext oldcontext;
	int			initial = pg_batch_row_count(batch);
	const Bitmapset *filter_columns = state->request->filter_columns;

	if (state->heap_scan_mode == PG_BATCH_HEAP_BITMAP &&
		!state->page_recheck)
		filter_columns = state->exact_filter_columns;
	pg_batch_prepare_columns(batch, filter_columns,
							 batch->selection, PG_BATCH_FILTER_PHASE);
	ResetExprContext(econtext);
	oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_tuple_memory);
	for (int q = 0; q < state->nquals && pg_batch_has_rows(batch); q++)
	{
		BatchQualState *qual = &state->quals[q];
		NullableDatum *other = &qual->fcinfo->args[1 - qual->var_argno];

		if (qual->recheck_only && !state->page_recheck)
		{
			state->exact_rechecks_skipped += pg_batch_row_count(batch);
			continue;
		}

		if (qual->other_expr != NULL)
			other->value = ExecEvalExpr(qual->other_expr, econtext,
										&other->isnull);
		else
			*other = qual->constant;
		if (other->isnull)
			MemSet(batch->selection, 0, sizeof(uint64) * batch->nwords);
		else if (qual->has_int4_kernel)
		{
			PgBatchInt4Vector column;

			pg_batch_get_int4_vector(batch, qual->column, batch->selection,
									 PG_BATCH_FILTER_PHASE, &column);
			pg_batch_filter_int4(&column, batch->nrows, batch->nwords,
								 batch->selection, qual->int4_op,
								 DatumGetInt32(other->value),
								 pg_batch_enable_simd);
		}
		else
		{
			MemoryContext oldoperator;
			bool		track_function =
				pgstat_track_functions > qual->fcinfo->flinfo->fn_stats;

			oldoperator = MemoryContextSwitchTo(state->operator_context);

			/*
			 * Keep the planner's qual order, but run each qual over the
			 * batch.
			 */
			{
				PgBatchArrowView column;

				if (unlikely(batch->ops->get_native_interface != NULL) &&
					pg_batch_get_arrow_column(batch, qual->column, &column))
				{
					const struct ArrowArray *array = column.array;
					const int32 *values = array->buffers[1];

					Assert(strcmp(column.schema->format, "i") == 0);
					for (int word = 0; word < batch->nwords; word++)
					{
						uint64		rows = pg_batch_selection_word(batch, word);

						while (rows != 0)
						{
							int			bitno = pg_rightmost_one_pos64(rows);
							uint64		bit = UINT64CONST(1) << bitno;
							int			row = word * 64 + bitno;

							if (!pg_batch_arrow_row_is_valid(array, row) ||
								!eval_qual(qual,
										   Int32GetDatum(values[array->offset + row]),
										   track_function))
								batch->selection[word] &= ~bit;
							rows &= ~bit;
						}
					}
					MemoryContextSwitchTo(oldoperator);
					goto generic_done;
				}
			}
			{
				PgBatchBridgeDatumColumn column;

				pg_batch_get_datum_column(batch, qual->column,
										  batch->selection,
										  PG_BATCH_FILTER_PHASE, &column);
				for (int word = 0; word < batch->nwords; word++)
				{
					uint64		rows = pg_batch_selection_word(batch, word);

					if (word >= column.nwords ||
						(column.valid_rows[word] & rows) != rows)
						elog(ERROR, "pg_batch source did not materialize filter column %d",
							 qual->column + 1);
					while (rows != 0)
					{
						int			bitno = pg_rightmost_one_pos64(rows);
						uint64		bit = UINT64CONST(1) << bitno;
						int			row = word * 64 + bitno;

						if (column.isnull[row] ||
							!eval_qual(qual, column.values[row], track_function))
							batch->selection[word] &= ~bit;
						rows &= ~bit;
					}
				}
			}
			MemoryContextSwitchTo(oldoperator);
		}
generic_done:
		if (qual->other_expr != NULL)
		{
			other->value = (Datum) 0;
			other->isnull = true;
		}
		MemoryContextReset(state->operator_context);
	}
	MemoryContextSwitchTo(oldcontext);
	{
		int			remaining = pg_batch_row_count(batch);

		InstrCountFiltered1(&state->css.ss, initial - remaining);
		return remaining;
	}
}

static TupleTableSlot *
scan_exec(CustomScanState *node)
{
	BatchScanState *state = (BatchScanState *) node;
	PgBatchSlot *bslot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);
	PgBatchBridgeBatch *batch;

	if (!ScanDirectionIsForward(node->ss.ps.state->es_direction))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_batch supports only forward scans")));
	if (state->done)
		return ExecClearTuple(&bslot->base);
	batch = bslot->active_batch;
	if (batch != NULL)
	{
		if (!batch->consumed)
			elog(ERROR, "pg_batch consumer requested a new batch too early");
		ExecClearTuple(&bslot->base);
	}

	while (next_batch(state, bslot))
	{
		int			rows;

		rows = filter_batch(state, bslot);
		batch = pg_batch_get_batch(&bslot->base);
		if (rows == 0)
		{
			pg_batch_finish_batch(&bslot->base);
			ExecClearTuple(&bslot->base);
			continue;
		}
		pg_batch_select_row(&bslot->base,
							pg_batch_bridge_next_selected(batch, -1));
		if (node->ss.ps.instrument != NULL)
			node->ss.ps.instrument->tuplecount += rows - 1;
		return &bslot->base;
	}
	return ExecClearTuple(&bslot->base);
}

static void
bitmap_end(BatchScanState *state)
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
scan_end(CustomScanState *node)
{
	BatchScanState *state = (BatchScanState *) node;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	bitmap_end(state);
	if (state->scan != NULL)
		table_endscan(state->scan);
	if (state->provider != NULL && state->provider_state != NULL)
		state->provider->end_scan(state->provider_state);
	state->provider_state = NULL;
	pg_batch_end_children(node);
}

static void
scan_rescan(CustomScanState *node)
{
	BatchScanState *state = (BatchScanState *) node;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	bitmap_end(state);
	if (state->scan != NULL)
		table_rescan(state->scan, NULL);
	if (state->provider != NULL && state->provider_state != NULL)
		state->provider->rescan(state->provider_state);
	if (state->bitmap_plan != NULL && state->bitmap_plan->chgParam == NULL)
		ExecReScan(state->bitmap_plan);
	state->page_active = false;
	state->page_recheck = false;
	state->first_batch_on_page = false;
	state->next_page_row = 0;
	state->done = false;
}

static void
scan_explain(CustomScanState *node, List *ancestors,
			 ExplainState *es)
{
	BatchScanState *state = (BatchScanState *) node;
	PgBatchSlot *bslot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);

	ExplainPropertyInteger("Batch Size", NULL, PG_BATCH_SIZE, es);
	ExplainPropertyText("Batch Source",
						state->provider != NULL ? state->provider->provider_name :
						state->heap_scan_mode == PG_BATCH_HEAP_BITMAP ?
						"Heap Bitmap" : "Heap", es);
	if (state->provider != NULL)
		ExplainPropertyText("Batch Provider", state->provider->provider_name, es);
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
			ExplainPropertyInteger("Exact Rechecks Skipped", NULL,
								   state->exact_rechecks_skipped, es);
		}
		if (state->provider != NULL && state->provider->explain != NULL)
			state->provider->explain(state->provider_state, es);
	}
}
