#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "common/int.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"

#include "internal.h"

typedef struct AggSpec
{
	PgBatchAggKind kind;
	int			column;
	int64		value;
	bool		has_value;
} AggSpec;

typedef struct BatchAggState
{
	CustomScanState css;
	PlanState  *child;
	AggSpec    *aggs;
	int			naggs;
	bool		emitted;
	uint64		input_batches;
	uint64		input_rows;
} BatchAggState;

static const CustomExecMethods pg_batch_agg_exec_methods;

Node *
pg_batch_create_agg_state(CustomScan *cscan)
{
	BatchAggState *state = palloc0_object(BatchAggState);

	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &pg_batch_agg_exec_methods;
	return (Node *) state;
}

static void agg_begin(CustomScanState *node, EState *estate,
					  int eflags);
static TupleTableSlot *agg_exec(CustomScanState *node);
static void agg_end(CustomScanState *node);
static void agg_rescan(CustomScanState *node);
static void agg_explain(CustomScanState *node, List *ancestors,
						ExplainState *es);

static const CustomExecMethods pg_batch_agg_exec_methods = {
	.CustomName = "PgBatchAgg",
	.BeginCustomScan = agg_begin,
	.ExecCustomScan = agg_exec,
	.EndCustomScan = agg_end,
	.ReScanCustomScan = agg_rescan,
	.ExplainCustomScan = agg_explain,
};

static TupleTableSlot *
child_batch_slot(PlanState *planstate)
{
	CustomScanState *custom = castNode(CustomScanState, planstate);

	return custom->ss.ss_ScanTupleSlot;
}

static int
find_source_column(const PgBatchBridgeRequest * request,
				   AttrNumber source_attnum)
{
	for (int i = 0; i < request->ncolumns; i++)
	{
		if (request->source_attnums[i] == source_attnum)
			return i;
	}
	return -1;
}

static void
reset_aggregate_values(BatchAggState *state)
{
	for (int i = 0; i < state->naggs; i++)
	{
		state->aggs[i].value = 0;
		state->aggs[i].has_value = false;
	}
	state->emitted = false;
}

static void
agg_begin(CustomScanState *node, EState *estate, int eflags)
{
	BatchAggState *state = (BatchAggState *) node;
	CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
	ListCell   *lc;
	TupleTableSlot *child_slot;
	const		PgBatchBridgeRequest *request;
	Bitmapset  *survivor_columns;
	int			i = 0;

	pg_batch_init_children(node, estate, eflags);
	state->child = linitial(node->custom_ps);
	child_slot = child_batch_slot(state->child);
	request = pg_batch_bridge->get_request(
										   pg_batch_slot_cast(child_slot)->binding);
	survivor_columns = bms_copy(request->survivor_columns);

	state->naggs = list_length(cscan->custom_private);
	state->aggs = palloc0_array(AggSpec, state->naggs);
	foreach(lc, cscan->custom_private)
	{
		List	   *item = lfirst_node(List, lc);
		AggSpec    *agg = &state->aggs[i++];
		AttrNumber	source_attnum = intVal(lsecond(item));

		agg->kind = intVal(linitial(item));
		agg->column = agg->kind == PG_BATCH_AGG_COUNT_STAR ? -1 :
			find_source_column(request, source_attnum);
		if (agg->kind != PG_BATCH_AGG_COUNT_STAR && agg->column < 0)
			elog(ERROR, "pg_batch aggregate input is missing from child slot");
		if (agg->column >= 0)
			survivor_columns = bms_add_member(survivor_columns, agg->column);
	}
	pg_batch_set_request(child_slot, request->filter_columns,
						 survivor_columns, true);
	bms_free(survivor_columns);
	reset_aggregate_values(state);
}

static void
add_int64_checked(int64 *target, int64 addend)
{
	int64		result;

	if (pg_add_s64_overflow(*target, addend, &result))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	*target = result;
}

static void
advance_aggregates(BatchAggState *state,
				   PgBatchBridgeBatch *batch, int selected)
{
	for (int i = 0; i < state->naggs; i++)
	{
		AggSpec    *agg = &state->aggs[i];

		if (agg->kind == PG_BATCH_AGG_COUNT_STAR)
		{
			add_int64_checked(&agg->value, selected);
			agg->has_value = true;
			continue;
		}

		{
			PgBatchArrowView column;

			if (unlikely(batch->ops->get_native_interface != NULL) &&
				pg_batch_get_arrow_column(batch, agg->column, &column))
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

						if (pg_batch_arrow_row_is_valid(array, row))
						{
							if (agg->kind == PG_BATCH_AGG_COUNT_COLUMN)
								add_int64_checked(&agg->value, 1);
							else
								add_int64_checked(&agg->value,
												  values[array->offset + row]);
							agg->has_value = true;
						}
						rows &= ~bit;
					}
				}
				continue;
			}
		}
		{
			PgBatchBridgeDatumColumn column;

			pg_batch_get_datum_column(batch, agg->column, batch->selection,
									  PG_BATCH_PROJECT_PHASE, &column);
			for (int word = 0; word < batch->nwords; word++)
			{
				uint64		rows = pg_batch_selection_word(batch, word);

				if (word >= column.nwords ||
					(column.valid_rows[word] & rows) != rows)
					elog(ERROR, "pg_batch source did not materialize aggregate column %d",
						 agg->column + 1);
				while (rows != 0)
				{
					int			bitno = pg_rightmost_one_pos64(rows);
					uint64		bit = UINT64CONST(1) << bitno;
					int			row = word * 64 + bitno;

					if (!column.isnull[row])
					{
						if (agg->kind == PG_BATCH_AGG_COUNT_COLUMN)
							add_int64_checked(&agg->value, 1);
						else
							add_int64_checked(&agg->value,
											  DatumGetInt32(column.values[row]));
						agg->has_value = true;
					}
					rows &= ~bit;
				}
			}
		}
	}
}

static TupleTableSlot *
agg_exec(CustomScanState *node)
{
	BatchAggState *state = (BatchAggState *) node;
	TupleTableSlot *result = node->ss.ss_ScanTupleSlot;

	if (state->emitted)
		return ExecClearTuple(result);
	for (;;)
	{
		TupleTableSlot *childslot;
		PgBatchBridgeBatch *batch;
		int			selected;

		childslot = ExecProcNode(state->child);
		if (TupIsNull(childslot))
			break;
		batch = pg_batch_get_batch(childslot);
		selected = pg_batch_row_count(batch);
		state->input_batches++;
		state->input_rows += selected;
		advance_aggregates(state, batch, selected);
		pg_batch_finish_batch(childslot);
	}

	ExecClearTuple(result);
	for (int i = 0; i < state->naggs; i++)
	{
		AggSpec    *agg = &state->aggs[i];

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
agg_end(CustomScanState *node)
{
	pg_batch_end_children(node);
}

static void
agg_rescan(CustomScanState *node)
{
	BatchAggState *state = (BatchAggState *) node;

	pg_batch_rescan_children(node);
	reset_aggregate_values(state);
}

static void
agg_explain(CustomScanState *node, List *ancestors,
			ExplainState *es)
{
	BatchAggState *state = (BatchAggState *) node;

	if (es->analyze)
	{
		ExplainPropertyInteger("Input Batches", NULL, state->input_batches, es);
		ExplainPropertyInteger("Input Rows", NULL, state->input_rows, es);
	}
}
