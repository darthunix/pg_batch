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
	/* Index into BatchAggState.inputs, or -1 for count(*). */
	int			input;
} AggSpec;

typedef struct AggInput
{
	int			column;
	Bitmapset  *column_mask;
	uint32		flags;
	PgBatchInt4Reduction value;
} AggInput;

typedef struct BatchAggState
{
	CustomScanState css;
	PlanState  *child;
	PgBatchInput *input;
	AggSpec    *aggs;
	AggInput   *inputs;
	int			naggs;
	int			ninputs;
	int64		count_star;
	bool		has_count_star;
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

static void
reset_aggregate_values(BatchAggState *state)
{
	state->count_star = 0;
	for (int i = 0; i < state->ninputs; i++)
		MemSet(&state->inputs[i].value, 0, sizeof(state->inputs[i].value));
	state->emitted = false;
}

static uint32
aggregate_flag(PgBatchAggKind kind)
{
	switch (kind)
	{
		case PG_BATCH_AGG_COUNT_COLUMN:
			return PG_BATCH_INT4_REDUCE_COUNT;
		case PG_BATCH_AGG_SUM_INT4:
			return PG_BATCH_INT4_REDUCE_SUM;
		case PG_BATCH_AGG_MIN_INT4:
			return PG_BATCH_INT4_REDUCE_MIN;
		case PG_BATCH_AGG_MAX_INT4:
			return PG_BATCH_INT4_REDUCE_MAX;
		case PG_BATCH_AGG_COUNT_STAR:
			break;
	}
	pg_unreachable();
}

static void
agg_begin(CustomScanState *node, EState *estate, int eflags)
{
	BatchAggState *state = (BatchAggState *) node;
	CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
	const char *producer_name = strVal(linitial(cscan->custom_private));
	List	   *specs = lsecond_node(List, cscan->custom_private);
	ListCell   *lc;
	PgBatchBridgeBinding *request_binding;
	const		PgBatchBridgeRequest *request;
	Bitmapset  *survivor_columns;
	int			i = 0;

	pg_batch_init_children(node, estate, eflags);
	state->child = linitial(node->custom_ps);
	state->input = pg_batch_input_create(estate->es_query_cxt,
		pg_batch_bridge, state->child, producer_name);
	request_binding = pg_batch_input_request_binding(state->input);
	request = pg_batch_bridge->get_request(request_binding);
	survivor_columns = bms_copy(request->survivor_columns);

	state->naggs = list_length(specs);
	state->aggs = palloc0_array(AggSpec, state->naggs);
	state->inputs = palloc0_array(AggInput, state->naggs);
	foreach(lc, specs)
	{
		List	   *item = lfirst_node(List, lc);
		AggSpec    *agg = &state->aggs[i++];
		int			column = intVal(lsecond(item));

		agg->kind = intVal(linitial(item));
		agg->input = -1;
		if (agg->kind == PG_BATCH_AGG_COUNT_STAR)
		{
			state->has_count_star = true;
			continue;
		}
		if (column < 0)
			elog(ERROR, "pg_batch aggregate input is missing from child slot");
		for (int input = 0; input < state->ninputs; input++)
		{
			if (state->inputs[input].column == column)
			{
				agg->input = input;
				break;
			}
		}
		if (agg->input < 0)
		{
			AggInput   *input = &state->inputs[state->ninputs];

			agg->input = state->ninputs++;
			input->column = column;
			input->column_mask = bms_make_singleton(column);
			survivor_columns = bms_add_member(survivor_columns, column);
		}
		state->inputs[agg->input].flags |= aggregate_flag(agg->kind);
	}
	pg_batch_bridge->set_request(request_binding, request->filter_columns,
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
merge_reduction(AggInput *input, const PgBatchInt4Reduction *partial)
{
	PgBatchInt4Reduction *value = &input->value;

	if (input->flags & PG_BATCH_INT4_REDUCE_COUNT)
		add_int64_checked(&value->count, partial->count);
	if (!partial->has_value)
		return;
	if (input->flags & PG_BATCH_INT4_REDUCE_SUM)
		add_int64_checked(&value->sum, partial->sum);
	if (!value->has_value)
	{
		if (input->flags & PG_BATCH_INT4_REDUCE_MIN)
			value->min = partial->min;
		if (input->flags & PG_BATCH_INT4_REDUCE_MAX)
			value->max = partial->max;
		value->has_value = true;
		return;
	}
	if ((input->flags & PG_BATCH_INT4_REDUCE_MIN) &&
		partial->min < value->min)
		value->min = partial->min;
	if ((input->flags & PG_BATCH_INT4_REDUCE_MAX) &&
		partial->max > value->max)
		value->max = partial->max;
}

static void
advance_aggregates(BatchAggState *state,
				   PgBatchBridgeBatch *batch, int selected)
{
	if (state->has_count_star)
		add_int64_checked(&state->count_star, selected);

	for (int i = 0; i < state->ninputs; i++)
	{
		AggInput   *input = &state->inputs[i];
		PgBatchInt4Vector column;
		PgBatchInt4Reduction partial;

		pg_batch_prepare_columns(batch, input->column_mask,
								 batch->selection, PG_BATCH_PROJECT_PHASE);
		pg_batch_get_int4_vector(batch, input->column, batch->selection,
								 PG_BATCH_PROJECT_PHASE, &column);
		pg_batch_reduce_int4(&column, batch->nrows, batch->nwords,
							 batch->selection, input->flags, &partial);
		merge_reduction(input, &partial);
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
		PgBatchInputBatch input;
		PgBatchBridgeBatch *batch;
		int			selected;

		if (!pg_batch_input_advance(state->input, &input))
			break;
		batch = input.batch;
		selected = pg_batch_row_count(batch);
		state->input_batches++;
		state->input_rows += selected;
		advance_aggregates(state, batch, selected);
	}

	ExecClearTuple(result);
	for (int i = 0; i < state->naggs; i++)
	{
		AggSpec    *agg = &state->aggs[i];
		PgBatchInt4Reduction *value =
			agg->input < 0 ? NULL : &state->inputs[agg->input].value;

		if (agg->kind != PG_BATCH_AGG_COUNT_STAR &&
			agg->kind != PG_BATCH_AGG_COUNT_COLUMN && !value->has_value)
		{
			result->tts_values[i] = (Datum) 0;
			result->tts_isnull[i] = true;
		}
		else
		{
			switch (agg->kind)
			{
				case PG_BATCH_AGG_COUNT_STAR:
					result->tts_values[i] = Int64GetDatum(state->count_star);
					break;
				case PG_BATCH_AGG_COUNT_COLUMN:
					result->tts_values[i] = Int64GetDatum(value->count);
					break;
				case PG_BATCH_AGG_SUM_INT4:
					result->tts_values[i] = Int64GetDatum(value->sum);
					break;
				case PG_BATCH_AGG_MIN_INT4:
					result->tts_values[i] = Int32GetDatum(value->min);
					break;
				case PG_BATCH_AGG_MAX_INT4:
					result->tts_values[i] = Int32GetDatum(value->max);
					break;
			}
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
	pg_batch_input_reset(state->input);
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
