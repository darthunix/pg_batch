#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "common/int.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"

#include "pg_batch.h"

typedef struct PgBatchAggSpec
{
	PgBatchAggKind kind;
	int			column;
	int64		value;
	bool		has_value;
} PgBatchAggSpec;

typedef struct PgBatchAggState
{
	CustomScanState css;
	PlanState  *child;
	PgBatchAggSpec *aggs;
	int			naggs;
	bool		emitted;
	uint64		input_batches;
	uint64		input_rows;
} PgBatchAggState;

static const CustomExecMethods pg_batch_agg_exec_methods;

Node *
pg_batch_create_agg_state(CustomScan *cscan)
{
	PgBatchAggState *state = palloc0_object(PgBatchAggState);

	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &pg_batch_agg_exec_methods;
	return (Node *) state;
}

static void pg_batch_agg_begin(CustomScanState *node, EState *estate,
							   int eflags);
static TupleTableSlot *pg_batch_agg_exec(CustomScanState *node);
static void pg_batch_agg_end(CustomScanState *node);
static void pg_batch_agg_rescan(CustomScanState *node);
static void pg_batch_agg_explain(CustomScanState *node, List *ancestors,
								 ExplainState *es);

static const CustomExecMethods pg_batch_agg_exec_methods = {
	.CustomName = "PgBatchAgg",
	.BeginCustomScan = pg_batch_agg_begin,
	.ExecCustomScan = pg_batch_agg_exec,
	.EndCustomScan = pg_batch_agg_end,
	.ReScanCustomScan = pg_batch_agg_rescan,
	.ExplainCustomScan = pg_batch_agg_explain,
};

static PgBatchSlot *
pg_batch_child_batch_slot(PlanState *planstate)
{
	CustomScanState *custom = castNode(CustomScanState, planstate);

	return pg_batch_slot_cast(custom->ss.ss_ScanTupleSlot);
}

static int
pg_batch_find_source_column(PgBatchSlot *slot, AttrNumber source_attnum)
{
	for (int i = 0; i < slot->ncolumns; i++)
	{
		if (slot->source_attnums[i] == source_attnum)
			return i;
	}
	return -1;
}

static void
pg_batch_agg_reset_values(PgBatchAggState *state)
{
	for (int i = 0; i < state->naggs; i++)
	{
		state->aggs[i].value = 0;
		state->aggs[i].has_value = false;
	}
	state->emitted = false;
}

static void
pg_batch_agg_begin(CustomScanState *node, EState *estate, int eflags)
{
	PgBatchAggState *state = (PgBatchAggState *) node;
	CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
	ListCell   *lc;
	PgBatchSlot *child_slot;
	Bitmapset  *survivor_columns;
	int			i = 0;

	pg_batch_init_children(node, estate, eflags);
	state->child = linitial(node->custom_ps);
	child_slot = pg_batch_child_batch_slot(state->child);
	survivor_columns = bms_copy(child_slot->request.survivor_columns);

	state->naggs = list_length(cscan->custom_private);
	state->aggs = palloc0_array(PgBatchAggSpec, state->naggs);
	foreach(lc, cscan->custom_private)
	{
		List	   *item = lfirst_node(List, lc);
		PgBatchAggSpec *agg = &state->aggs[i++];
		AttrNumber	source_attnum = intVal(lsecond(item));

		agg->kind = intVal(linitial(item));
		agg->column = agg->kind == PG_BATCH_AGG_COUNT_STAR ? -1 :
			pg_batch_find_source_column(child_slot, source_attnum);
		if (agg->kind != PG_BATCH_AGG_COUNT_STAR && agg->column < 0)
			elog(ERROR, "pg_batch aggregate input is missing from child slot");
		if (agg->column >= 0)
			survivor_columns = bms_add_member(survivor_columns, agg->column);
	}
	pg_batch_set_request(child_slot, child_slot->request.filter_columns,
						 survivor_columns, true);
	bms_free(survivor_columns);
	pg_batch_agg_reset_values(state);
}

static void
pg_batch_add_int64(int64 *target, int64 addend)
{
	int64		result;

	if (pg_add_s64_overflow(*target, addend, &result))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	*target = result;
}

static void
pg_batch_advance_aggregates(PgBatchAggState *state, PgBatchSlot *slot)
{
	uint64		selected_rows = slot->selected_rows;
	int			selected = pg_batch_row_count(selected_rows);

	for (int i = 0; i < state->naggs; i++)
	{
		PgBatchAggSpec *agg = &state->aggs[i];
		PgBatchColumn *column;
		uint64		rows;

		if (agg->kind == PG_BATCH_AGG_COUNT_STAR)
		{
			pg_batch_add_int64(&agg->value, selected);
			agg->has_value = true;
			continue;
		}

		column = &slot->columns[agg->column];
		rows = selected_rows;

		while (rows != 0)
		{
			int			row = pg_rightmost_one_pos64(rows);
			uint64		bit = UINT64CONST(1) << row;

			Assert((column->valid_rows & bit) != 0);
			if (!column->isnull[row])
			{
				if (agg->kind == PG_BATCH_AGG_COUNT_COLUMN)
					pg_batch_add_int64(&agg->value, 1);
				else
					pg_batch_add_int64(&agg->value,
									   DatumGetInt32(column->values[row]));
				agg->has_value = true;
			}
			rows &= ~bit;
		}
	}
}

static TupleTableSlot *
pg_batch_agg_exec(CustomScanState *node)
{
	PgBatchAggState *state = (PgBatchAggState *) node;
	TupleTableSlot *result = node->ss.ss_ScanTupleSlot;

	if (state->emitted)
		return ExecClearTuple(result);
	for (;;)
	{
		TupleTableSlot *childslot;
		PgBatchSlot *batch;

		childslot = ExecProcNode(state->child);
		if (TupIsNull(childslot))
			break;
		batch = pg_batch_slot_cast(childslot);
		state->input_batches++;
		state->input_rows += pg_batch_row_count(batch->selected_rows);
		pg_batch_advance_aggregates(state, batch);
		pg_batch_finish_batch(batch);
	}

	ExecClearTuple(result);
	for (int i = 0; i < state->naggs; i++)
	{
		PgBatchAggSpec *agg = &state->aggs[i];

		if (agg->kind == PG_BATCH_AGG_SUM_INT4 && !agg->has_value)
		{
			result->tts_values[i] = (Datum) 0;
			result->tts_isnull[i] = true;
		}
		else
		{
			result->tts_values[i] = Int64GetDatum(agg->value);
			result->tts_isnull[i] = false;
		}
	}
	ExecStoreVirtualTuple(result);
	state->emitted = true;
	return result;
}

static void
pg_batch_agg_end(CustomScanState *node)
{
	pg_batch_end_children(node);
}

static void
pg_batch_agg_rescan(CustomScanState *node)
{
	PgBatchAggState *state = (PgBatchAggState *) node;

	pg_batch_rescan_children(node);
	pg_batch_agg_reset_values(state);
}

static void
pg_batch_agg_explain(CustomScanState *node, List *ancestors,
					 ExplainState *es)
{
	PgBatchAggState *state = (PgBatchAggState *) node;

	if (es->analyze)
	{
		ExplainPropertyInteger("Input Batches", NULL, state->input_batches, es);
		ExplainPropertyInteger("Input Rows", NULL, state->input_rows, es);
	}
}
