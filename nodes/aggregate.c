#include "postgres.h"

#include "miscadmin.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "utils/hsearch.h"

#include "internal.h"
#include "spill.h"

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

typedef struct GroupKey
{
	int32		value;
	bool		isnull;
} GroupKey;

typedef struct GroupEntry
{
	GroupKey	key;
	int64		count_star;
	PgBatchInt4Reduction values[FLEXIBLE_ARRAY_MEMBER];
} GroupEntry;

typedef struct SpillWork
{
	PgBatchSpillSet *set;
	int			partition;
	int			hash_shift;
} SpillWork;

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
	bool		grouped;
	int			group_input;
	MemoryContext group_context;
	MemoryContext partition_context;
	HTAB	   *main_groups;
	HTAB	   *emit_groups;
	HASH_SEQ_STATUS group_seq;
	Size		group_entry_size;
	uint64		group_limit;
	Size		spill_buffer_limit;
	bool		input_done;
	bool		spilling;
	PgBatchSpillSet *initial_spill;
	List	   *spill_sets;
	List	   *spill_work;
	TupleDesc	spill_desc;
	PgBatchInt4Vector *batch_vectors;
	PgBatchInt4Vector *spill_vectors;
	uint32		hashes[PG_BATCH_SIZE];
	uint64		spill_rows;
	uint64		spill_partitions;
	uint64		spill_repartitions;
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
static HTAB *create_group_table(BatchAggState *state,
	MemoryContext context);
static void destroy_spills(BatchAggState *state);

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
	state->input_done = false;
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
		case PG_BATCH_AGG_GROUP_KEY:
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
	PgBatchBinding *request_binding;
	const		PgBatchRequest *request;
	Bitmapset  *project_columns;
	int			i = 0;

	pg_batch_init_children(node, estate, eflags);
	state->child = linitial(node->custom_ps);
	state->input = pg_batch_input_create(estate->es_query_cxt,
		pg_batch_api, state->child, producer_name);
	request_binding = pg_batch_input_request_binding(state->input);
	request = pg_batch_api->get_request(request_binding);
	project_columns = bms_copy(request->project_columns);

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
			project_columns = bms_add_member(project_columns, column);
		}
		if (agg->kind == PG_BATCH_AGG_GROUP_KEY)
		{
			if (state->grouped && state->group_input != agg->input)
				elog(ERROR, "pg_batch aggregate has more than one group key");
			if (!state->grouped)
			{
				state->grouped = true;
				state->group_input = agg->input;
			}
		}
		else
			state->inputs[agg->input].flags |= aggregate_flag(agg->kind);
	}
	pg_batch_api->set_request(request_binding, request->filter_columns,
						 project_columns, true);
	bms_free(project_columns);
	if (state->grouped)
	{
		TupleDesc	child_desc = ExecGetResultType(state->child);

		state->group_context = AllocSetContextCreate(estate->es_query_cxt,
			"pg_batch aggregate groups", ALLOCSET_DEFAULT_SIZES);
		state->partition_context = AllocSetContextCreate(estate->es_query_cxt,
			"pg_batch aggregate partition", ALLOCSET_DEFAULT_SIZES);
		state->group_entry_size = offsetof(GroupEntry, values) +
			sizeof(PgBatchInt4Reduction) * state->ninputs;
		state->group_limit = Max((uint64) 1,
			(uint64) get_hash_memory_limit() /
			(state->group_entry_size + 64));
		state->spill_buffer_limit = Max((Size) 1024,
			(Size) get_hash_memory_limit() / 2);
		state->main_groups = create_group_table(state, state->group_context);
		state->batch_vectors = palloc_array(PgBatchInt4Vector, state->ninputs);
		state->spill_vectors = palloc_array(PgBatchInt4Vector, state->ninputs);
		state->spill_desc = CreateTemplateTupleDesc(state->ninputs);
		for (int column = 0; column < state->ninputs; column++)
			TupleDescInitEntry(state->spill_desc, column + 1, NULL,
				INT4OID, -1, 0);
		TupleDescFinalize(state->spill_desc);
		/* Keep the child descriptor check close to executor initialization. */
		if (child_desc == NULL)
			elog(ERROR, "pg_batch aggregate child has no tuple descriptor");
	}
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

static HTAB *
create_group_table(BatchAggState *state, MemoryContext context)
{
	HASHCTL		ctl;
	MemoryContext oldcontext;
	HTAB	   *groups;

	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(GroupKey);
	ctl.entrysize = state->group_entry_size;
	ctl.hcxt = context;
	oldcontext = MemoryContextSwitchTo(context);
	groups = hash_create("pg_batch aggregate groups", 1024, &ctl,
					 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	MemoryContextSwitchTo(oldcontext);
	return groups;
}

static GroupEntry *
find_group(BatchAggState *state, HTAB *groups, GroupKey key,
		   bool allow_insert, bool *found)
{
	GroupEntry *entry;

	entry = hash_search(groups, &key,
		allow_insert ? HASH_ENTER : HASH_FIND, found);
	if (entry != NULL && !*found)
	{
		entry->count_star = 0;
		MemSet(entry->values, 0,
			   sizeof(PgBatchInt4Reduction) * state->ninputs);
	}
	return entry;
}

static void
advance_group_entry(BatchAggState *state, GroupEntry *entry,
					const PgBatchInt4Vector *vectors, int row)
{
	if (state->has_count_star)
		add_int64_checked(&entry->count_star, 1);
	for (int inputno = 0; inputno < state->ninputs; inputno++)
	{
		AggInput   *input = &state->inputs[inputno];
		PgBatchInt4Reduction *value = &entry->values[inputno];
		int32		input_value;

		if (input->flags == 0 ||
			pg_batch_int4_row_is_null(&vectors[inputno], row))
			continue;
		input_value = pg_batch_int4_row_value(&vectors[inputno], row);
		if (input->flags & PG_BATCH_INT4_REDUCE_COUNT)
			add_int64_checked(&value->count, 1);
		if (input->flags & PG_BATCH_INT4_REDUCE_SUM)
			add_int64_checked(&value->sum, input_value);
		if (!value->has_value)
		{
			value->min = input_value;
			value->max = input_value;
			value->has_value = true;
		}
		else
		{
			if ((input->flags & PG_BATCH_INT4_REDUCE_MIN) &&
				input_value < value->min)
				value->min = input_value;
			if ((input->flags & PG_BATCH_INT4_REDUCE_MAX) &&
				input_value > value->max)
				value->max = input_value;
		}
	}
}

static void
load_group_vectors(BatchAggState *state, PgBatch *batch)
{
	for (int inputno = 0; inputno < state->ninputs; inputno++)
	{
		AggInput   *input = &state->inputs[inputno];

		pg_batch_prepare_columns(batch, input->column_mask,
			batch->selection, PG_BATCH_PROJECT_PHASE);
		pg_batch_get_int4_vector(batch, input->column, batch->selection,
			PG_BATCH_PROJECT_PHASE, &state->batch_vectors[inputno]);
	}
}

static uint32
group_hash(const PgBatchInt4Vector *key, int row, GroupKey *result)
{
	MemSet(result, 0, sizeof(*result));
	result->isnull = pg_batch_int4_row_is_null(key, row);
	if (result->isnull)
		return murmurhash32(UINT32_C(0x9e3779b9));
	result->value = pg_batch_int4_row_value(key, row);
	return murmurhash32((uint32) result->value);
}

static void
ensure_initial_spill(BatchAggState *state)
{
	if (state->initial_spill != NULL)
		return;
	state->initial_spill = pg_batch_spill_create(
		state->css.ss.ps.state->es_query_cxt, state->spill_desc,
		state->ninputs, 32, state->spill_buffer_limit);
	state->spill_sets = lappend(state->spill_sets, state->initial_spill);
	state->spill_partitions = 32;
}

static void
advance_grouped_batch(BatchAggState *state, PgBatch *batch)
{
	uint64		partition_rows[32] = {0};
	uint64		rows;
	const PgBatchInt4Vector *key;

	if (batch->nwords != 1 || batch->nrows > PG_BATCH_SIZE)
		elog(ERROR, "pg_batch grouped aggregate requires batches of at most 64 rows");
	load_group_vectors(state, batch);
	key = &state->batch_vectors[state->group_input];
	rows = batch->selection[0];
	while (rows != 0)
	{
		int			row = pg_rightmost_one_pos64(rows);
		GroupKey	group_key;
		GroupEntry *entry;
		bool		found;
		uint32		hash = group_hash(key, row, &group_key);

		state->hashes[row] = hash;
		entry = find_group(state, state->main_groups, group_key, false, &found);
		if (entry == NULL && !state->spilling &&
			hash_get_num_entries(state->main_groups) < state->group_limit)
			entry = find_group(state, state->main_groups, group_key, true, &found);
		if (entry != NULL)
			advance_group_entry(state, entry, state->batch_vectors, row);
		else
		{
			int			partition = hash & 31;

			state->spilling = true;
			partition_rows[partition] |= UINT64CONST(1) << row;
			state->spill_rows++;
		}
		rows &= rows - 1;
	}
	if (!state->spilling)
		return;
	ensure_initial_spill(state);
	for (int partition = 0; partition < 32; partition++)
	{
		if (partition_rows[partition] != 0)
			pg_batch_spill_write(state->initial_spill, partition,
				state->batch_vectors, batch->nrows,
				&partition_rows[partition], state->hashes);
	}
}

static void
advance_aggregates(BatchAggState *state,
				   PgBatch *batch, int selected)
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

static void
spill_block_vectors(BatchAggState *state, const PgBatchSpillBlock *block)
{
	for (int column = 0; column < state->ninputs; column++)
		pg_batch_int4_vector_init_packed(&state->spill_vectors[column],
			&block->values[column * PG_BATCH_SPILL_BLOCK_ROWS],
			&block->validity[column * 8], 0);
}

static bool
load_spill_groups(BatchAggState *state, SpillWork *work, HTAB *groups)
{
	PgBatchSpillReader *reader =
		pg_batch_spill_reader_open(work->set, work->partition);
	PgBatchSpillBlock block;

	while (pg_batch_spill_reader_next(reader, &block))
	{
		uint64		rows = block.selection;

		spill_block_vectors(state, &block);
		while (rows != 0)
		{
			int			row = pg_rightmost_one_pos64(rows);
			GroupKey	key;
			GroupEntry *entry;
			bool		found;

			(void) group_hash(&state->spill_vectors[state->group_input], row,
				&key);
			entry = find_group(state, groups, key, false, &found);
			if (entry == NULL && hash_get_num_entries(groups) >= state->group_limit)
			{
				pg_batch_spill_reader_close(reader);
				return false;
			}
			if (entry == NULL)
				entry = find_group(state, groups, key, true, &found);
			advance_group_entry(state, entry, state->spill_vectors, row);
			rows &= rows - 1;
		}
	}
	pg_batch_spill_reader_close(reader);
	return true;
}

static void
append_spill_work(BatchAggState *state, PgBatchSpillSet *set,
				  int partition, int hash_shift)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(
		state->css.ss.ps.state->es_query_cxt);
	SpillWork  *work = palloc_object(SpillWork);

	work->set = set;
	work->partition = partition;
	work->hash_shift = hash_shift;
	state->spill_work = lappend(state->spill_work, work);
	MemoryContextSwitchTo(oldcontext);
}

static void
repartition_spill(BatchAggState *state, SpillWork *work)
{
	PgBatchSpillReader *reader;
	PgBatchSpillBlock block;
	PgBatchSpillSet *child;
	int			hash_bits;
	int			npartitions;

	if (work->hash_shift >= 32)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("pg_batch aggregate cannot repartition a skewed group set")));
	hash_bits = Min(5, 32 - work->hash_shift);
	npartitions = 1 << hash_bits;
	child = pg_batch_spill_create(state->css.ss.ps.state->es_query_cxt,
		state->spill_desc, state->ninputs, npartitions,
		state->spill_buffer_limit);
	state->spill_sets = lappend(state->spill_sets, child);
	reader = pg_batch_spill_reader_open(work->set, work->partition);
	while (pg_batch_spill_reader_next(reader, &block))
	{
		uint64		partition_rows[32] = {0};
		uint64		rows = block.selection;

		spill_block_vectors(state, &block);
		while (rows != 0)
		{
			int			row = pg_rightmost_one_pos64(rows);
			int			partition =
				(block.hashes[row] >> work->hash_shift) & (npartitions - 1);

			partition_rows[partition] |= UINT64CONST(1) << row;
			rows &= rows - 1;
		}
		for (int partition = 0; partition < npartitions; partition++)
		{
			if (partition_rows[partition] != 0)
				pg_batch_spill_write(child, partition,
					state->spill_vectors, block.nrows,
					&partition_rows[partition], block.hashes);
		}
	}
	pg_batch_spill_reader_close(reader);
	pg_batch_spill_finish(child, true);
	state->spill_repartitions++;
	state->spill_partitions += npartitions;
	for (int partition = 0; partition < npartitions; partition++)
	{
		if (!pg_batch_spill_partition_empty(child, partition))
			append_spill_work(state, child, partition,
				work->hash_shift + hash_bits);
	}
}

static bool
prepare_next_spill_partition(BatchAggState *state)
{
	while (state->spill_work != NIL)
	{
		SpillWork  *work = linitial(state->spill_work);
		HTAB	   *groups;

		state->spill_work = list_delete_first(state->spill_work);
		MemoryContextReset(state->partition_context);
		groups = create_group_table(state, state->partition_context);
		if (!load_spill_groups(state, work, groups))
		{
			repartition_spill(state, work);
			continue;
		}
		state->emit_groups = groups;
		hash_seq_init(&state->group_seq, groups);
		return true;
	}
	state->emit_groups = NULL;
	return false;
}

static void
finish_group_input(BatchAggState *state)
{
	state->input_done = true;
	if (state->initial_spill != NULL)
	{
		pg_batch_spill_finish(state->initial_spill, true);
		for (int partition = 0; partition < 32; partition++)
		{
			if (!pg_batch_spill_partition_empty(state->initial_spill,
					partition))
				append_spill_work(state, state->initial_spill, partition, 5);
		}
	}
	state->emit_groups = state->main_groups;
	hash_seq_init(&state->group_seq, state->main_groups);
}

static void
store_group_result(BatchAggState *state, TupleTableSlot *result,
				   GroupEntry *entry)
{
	ExecClearTuple(result);
	for (int i = 0; i < state->naggs; i++)
	{
		AggSpec    *agg = &state->aggs[i];
		PgBatchInt4Reduction *value =
			agg->input < 0 ? NULL : &entry->values[agg->input];

		result->tts_isnull[i] = false;
		switch (agg->kind)
		{
			case PG_BATCH_AGG_GROUP_KEY:
				result->tts_values[i] = Int32GetDatum(entry->key.value);
				result->tts_isnull[i] = entry->key.isnull;
				break;
			case PG_BATCH_AGG_COUNT_STAR:
				result->tts_values[i] = Int64GetDatum(entry->count_star);
				break;
			case PG_BATCH_AGG_COUNT_COLUMN:
				result->tts_values[i] = Int64GetDatum(value->count);
				break;
			case PG_BATCH_AGG_SUM_INT4:
				result->tts_values[i] = Int64GetDatum(value->sum);
				result->tts_isnull[i] = !value->has_value;
				break;
			case PG_BATCH_AGG_MIN_INT4:
				result->tts_values[i] = Int32GetDatum(value->min);
				result->tts_isnull[i] = !value->has_value;
				break;
			case PG_BATCH_AGG_MAX_INT4:
				result->tts_values[i] = Int32GetDatum(value->max);
				result->tts_isnull[i] = !value->has_value;
				break;
		}
	}
	ExecStoreVirtualTuple(result);
}

static TupleTableSlot *
agg_exec_grouped(BatchAggState *state)
{
	TupleTableSlot *result = state->css.ss.ss_ScanTupleSlot;

	if (!state->input_done)
	{
		for (;;)
		{
			PgBatchInputBatch input;
			int			selected;

			if (!pg_batch_input_next(state->input, &input))
				break;
			selected = pg_batch_row_count(input.batch);
			state->input_batches++;
			state->input_rows += selected;
			advance_grouped_batch(state, input.batch);
			pg_batch_input_finish(state->input);
		}
		finish_group_input(state);
	}
	for (;;)
	{
		GroupEntry *entry = hash_seq_search(&state->group_seq);

		if (entry != NULL)
		{
			store_group_result(state, result, entry);
			return result;
		}
		if (!prepare_next_spill_partition(state))
			return ExecClearTuple(result);
	}
}

static TupleTableSlot *
agg_exec(CustomScanState *node)
{
	BatchAggState *state = (BatchAggState *) node;
	TupleTableSlot *result = node->ss.ss_ScanTupleSlot;

	if (state->grouped)
		return agg_exec_grouped(state);
	if (state->emitted)
		return ExecClearTuple(result);
	for (;;)
	{
		PgBatchInputBatch input;
		PgBatch *batch;
		int			selected;

		if (!pg_batch_input_next(state->input, &input))
			break;
		batch = input.batch;
		selected = pg_batch_row_count(batch);
		state->input_batches++;
		state->input_rows += selected;
		advance_aggregates(state, batch, selected);
		pg_batch_input_finish(state->input);
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
				case PG_BATCH_AGG_GROUP_KEY:
					pg_unreachable();
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
	BatchAggState *state = (BatchAggState *) node;

	destroy_spills(state);
	pg_batch_end_children(node);
}

static void
destroy_spills(BatchAggState *state)
{
	foreach_ptr(PgBatchSpillSet, set, state->spill_sets)
		pg_batch_spill_destroy(set);
	list_free(state->spill_sets);
	state->spill_sets = NIL;
	state->spill_work = NIL;
	state->initial_spill = NULL;
}

static void
agg_rescan(CustomScanState *node)
{
	BatchAggState *state = (BatchAggState *) node;

	pg_batch_rescan_children(node);
	pg_batch_input_rescan(state->input);
	if (state->grouped)
	{
		destroy_spills(state);
		MemoryContextReset(state->group_context);
		MemoryContextReset(state->partition_context);
		state->main_groups = create_group_table(state, state->group_context);
		state->emit_groups = NULL;
		state->spilling = false;
		state->spill_rows = 0;
		state->spill_partitions = 0;
		state->spill_repartitions = 0;
	}
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
		if (state->grouped)
		{
			ExplainPropertyInteger("Resident Groups", NULL,
				hash_get_num_entries(state->main_groups), es);
			ExplainPropertyInteger("Spill Rows", NULL,
				state->spill_rows, es);
			ExplainPropertyInteger("Spill Partitions", NULL,
				state->spill_partitions, es);
			ExplainPropertyInteger("Spill Repartitions", NULL,
				state->spill_repartitions, es);
		}
	}
}
