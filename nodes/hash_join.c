#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"

#include "hash_join_internal.h"

/*
 * This is a deliberately small hash-join experiment, not a replacement for
 * nodeHashjoin.c. It accepts only direct int4 equality keys, keeps the build
 * side in column arrays, and probes up to 64 active rows at a time. A result
 * batch initially contains only (probe row, build row) pairs. Its projected
 * columns are gathered later, when the parent requests them.
 *
 * Exact build keys occupy an open-addressed table. Only true duplicates use
 * linked lists, so unrelated bucket collisions do not add another comparison
 * pass over a probe window. When the build keys are unique, lookup and result
 * pair creation are fused. Direct int4 residual conditions also run over the
 * candidate batch before the scalar expression fallback.
 *
 * If the build estimate exceeds PostgreSQL's hash memory limit, both sides
 * are partitioned into private BufFile blocks. Small writes are combined into
 * 64-row blocks. A partition requiring many build chunks is partitioned again
 * with unused hash bits. Remaining skew falls back to bounded build chunks
 * and probe rescans. The spill representation never crosses the slot bridge.
 */

static const CustomExecMethods hash_join_exec_methods;
static const PgBatchBridgeBatchOps hash_join_batch_ops;

static void hash_join_begin(CustomScanState *node, EState *estate,
							int eflags);
static TupleTableSlot *hash_join_exec(CustomScanState *node);
static void hash_join_end(CustomScanState *node);
static void hash_join_rescan(CustomScanState *node);
static void hash_join_explain(CustomScanState *node, List *ancestors,
							  ExplainState *es);

Node *
pg_batch_create_hash_join_state(CustomScan *cscan)
{
	BatchHashJoinState *state = palloc0_object(BatchHashJoinState);

	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &hash_join_exec_methods;
	return (Node *) state;
}

TupleTableSlot *
pg_batch_hash_join_output_slot(CustomScanState *node)
{
	if (node->methods != &hash_join_exec_methods)
		return NULL;
	return ((BatchHashJoinState *) node)->output_slot;
}

static const CustomExecMethods hash_join_exec_methods = {
	.CustomName = "PgBatchHashJoin",
	.BeginCustomScan = hash_join_begin,
	.ExecCustomScan = hash_join_exec,
	.EndCustomScan = hash_join_end,
	.ReScanCustomScan = hash_join_rescan,
	.ExplainCustomScan = hash_join_explain,
};

static void
request_side_columns(PgBatchBridgeBinding *binding,
					 const int *columns, int ncolumns,
					 const int *keys, int nkeys)
{
	const PgBatchBridgeRequest *request =
		pg_batch_bridge->get_request(binding);
	Bitmapset  *filters = bms_copy(request->filter_columns);
	Bitmapset  *survivors = bms_copy(request->survivor_columns);
	bool	   *key_columns = palloc0_array(bool, ncolumns);

	for (int key = 0; key < nkeys; key++)
	{
		key_columns[keys[key]] = true;
		filters = bms_add_member(filters, columns[keys[key]]);
	}
	for (int column = 0; column < ncolumns; column++)
	{
		if (!key_columns[column])
			survivors = bms_add_member(survivors, columns[column]);
	}
	pg_batch_bridge->set_request(binding, filters, survivors, true);
	bms_free(filters);
	bms_free(survivors);
	pfree(key_columns);
}

static void
bind_input_column(PgBatchBridgeBatch *batch, int column, const uint64 *rows,
				  PgBatchMaterializePhase phase, InputColumn *result)
{
	pg_batch_get_int4_vector(batch, column, rows, phase, &result->vector);
	result->prepared = true;
}

void
hash_join_load_input_column(PgBatchBridgeBatch *batch, int column,
				  const uint64 *rows, PgBatchMaterializePhase phase,
				  InputColumn *result, MemoryContext scratch_context)
{
	Bitmapset  *columns;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(scratch_context);
	columns = bms_make_singleton(column);
	MemoryContextSwitchTo(oldcontext);
	pg_batch_prepare_columns(batch, columns, rows, phase);
	oldcontext = MemoryContextSwitchTo(scratch_context);
	bms_free(columns);
	MemoryContextSwitchTo(oldcontext);
	bind_input_column(batch, column, rows, phase, result);
}

static void
ensure_input_row(PgBatchBridgeBatch *batch, int column, int row,
				 InputColumn *input, PgBatchMaterializePhase phase,
				 MemoryContext scratch_context)
{
	uint64		row_word = UINT64CONST(1) << (row % 64);
	uint64	   *rows;
	uint64	   *allocated = NULL;

	if (input->prepared)
	{
		if (input->vector.layout == PG_BATCH_INT4_PACKED)
			return;
		if (pg_batch_int4_row_available(&input->vector, row))
			return;
	}
	if (batch->nwords == 1)
		rows = &row_word;
	else
	{
		allocated = MemoryContextAllocZero(scratch_context,
										  sizeof(uint64) * batch->nwords);
		allocated[row / 64] = row_word;
		rows = allocated;
	}
	hash_join_load_input_column(batch, column, rows, phase, input,
								scratch_context);
	if (allocated != NULL)
		pfree(allocated);
}

static uint64
hash_input_batch(BatchHashJoinState *state, PgBatchBridgeBatch *batch,
				 InputColumn *columns, const int *keys, uint32 *hashes)
{
	uint64		hash_rows;

	Assert(batch->nrows <= PG_BATCH_SIZE);
	Assert(batch->nwords == 1);
	for (int key = 0; key < state->nkeys; key++)
	{
		Assert(columns[keys[key]].prepared);
		state->hash_keys[key] = &columns[keys[key]].vector;
	}
	pg_batch_hash_int4(state->hash_keys, state->nkeys,
					   batch->nrows, batch->nwords, batch->selection,
					   hashes, &hash_rows);
	return hash_rows;
}

void
hash_join_reset_build_store(BuildStore *store)
{
	MemoryContextReset(store->context);
	store->columns = MemoryContextAllocZero(store->context,
											sizeof(StoredColumn) * store->ncolumns);
	store->links = NULL;
	store->buckets = NULL;
	store->nrows = 0;
	store->capacity = 0;
	store->nbuckets = 0;
	store->has_duplicates = false;
}

void
hash_join_track_peak_memory(BatchHashJoinState *state)
{
	state->peak_memory = Max(state->peak_memory,
		MemoryContextMemAllocated(state->join_context, true));
}

static void
resize_build_store(BuildStore *store, uint32 new_capacity)
{
	uint32		old_capacity = store->capacity;
	uint32		old_words = (old_capacity + 63) / 64;
	uint32		new_words = (new_capacity + 63) / 64;

	if (new_capacity <= old_capacity)
		elog(ERROR, "pg_batch hash join build side is too large");
	if (old_capacity == 0)
		store->links = MemoryContextAlloc(store->context,
										   sizeof(BuildLink) * new_capacity);
	else
		store->links = repalloc_array(store->links, BuildLink, new_capacity);
	for (int column = 0; column < store->ncolumns; column++)
	{
		StoredColumn *stored = &store->columns[column];

		if (old_capacity == 0)
		{
			stored->values = MemoryContextAlloc(store->context,
											 sizeof(int32) * new_capacity);
			stored->validity = MemoryContextAllocZero(store->context,
													  sizeof(uint64) * new_words);
		}
		else
		{
			stored->values = repalloc_array(stored->values, int32, new_capacity);
			stored->validity = repalloc_array(stored->validity, uint64,
													new_words);
			MemSet(stored->validity + old_words, 0,
				   sizeof(uint64) * (new_words - old_words));
		}
	}
	store->capacity = new_capacity;
}

void
hash_join_grow_build_store(BuildStore *store)
{
	resize_build_store(store,
					   store->capacity == 0 ? 1024 : store->capacity * 2);
}

static void
append_build_row_hash(BatchHashJoinState *state, InputColumn *columns, int row,
					  uint32 hash)
{
	BuildStore *store = &state->build;
	uint32		stored_row;

	if (store->nrows == store->capacity)
		hash_join_grow_build_store(store);
	stored_row = store->nrows++;
	store->links[stored_row].hash = hash;
	for (int column = 0; column < store->ncolumns; column++)
	{
		bool		isnull;
		int32		value = hash_join_input_value(&columns[column], row, &isnull);

		store->columns[column].values[stored_row] = value;
		if (!isnull)
			store->columns[column].validity[stored_row / 64] |=
				UINT64CONST(1) << (stored_row % 64);
	}
}

static void
limit_resident_build(BatchHashJoinState *state)
{
	while (!bms_is_empty(state->resident_partitions) &&
		   hash_join_estimated_build_memory(state->build.nrows,
										state->build.ncolumns) >
		   state->resident_memory_limit)
	{
		int			largest = -1;
		uint64		largest_rows = 0;
		int			partition = -1;

		while ((partition = bms_next_member(state->resident_partitions,
											 partition)) >= 0)
		{
			if (state->resident_partition_rows[partition] > largest_rows)
			{
				largest = partition;
				largest_rows = state->resident_partition_rows[partition];
			}
		}
		if (largest < 0)
			break;
		state->resident_partitions =
			bms_del_member(state->resident_partitions, largest);
		hash_join_spill_build_store(state);
	}
}

static uint64
append_or_spill_build_batch(BatchHashJoinState *state,
							PgBatchBridgeBatch *batch, InputColumn *columns,
							const uint32 *hashes, uint64 hash_rows)
{
	uint64		resident_rows = 0;
	uint64		accepted;
	uint64		rows;

	accepted = hash_join_spill_input_batch(state, batch, columns,
										state->build.ncolumns,
										hashes, hash_rows,
										NULL,
										state->build_files,
										&resident_rows);
	rows = resident_rows;
	while (rows != 0)
	{
		int			row = pg_rightmost_one_pos64(rows);
		uint64		bit = UINT64CONST(1) << row;
		int			partition;

		partition = hashes[row] & (state->npartitions - 1);
		append_build_row_hash(state, columns, row, hashes[row]);
		state->resident_partition_rows[partition]++;
		state->resident_build_rows++;
		rows &= ~bit;
	}
	limit_resident_build(state);
	return accepted;
}

static uint32
choose_bucket_count(uint32 nrows)
{
	uint32		result = 1024;

	while ((uint64) result < (uint64) nrows * 2 &&
		   result < (UINT32_MAX / 2 + 1))
		result <<= 1;
	return result;
}

static bool
build_keys_equal(BatchHashJoinState *state, uint32 left, uint32 right)
{
	BuildStore *store = &state->build;

	/*
	 * murmurhash32() is a permutation of uint32 values, and hash_combine()
	 * is reversible in its second argument. Equal combined hashes plus an
	 * equal key prefix therefore prove that the final key is also equal.
	 */
	for (int key = 0; key < state->nkeys - 1; key++)
	{
		int			column = state->inner_keys[key];

		if (store->columns[column].values[left] !=
			store->columns[column].values[right])
			return false;
	}
	return true;
}

void
hash_join_finish_build_table(BatchHashJoinState *state)
{
	BuildStore *store = &state->build;

	store->nbuckets = choose_bucket_count(store->nrows);
	store->buckets = MemoryContextAllocZero(store->context,
											 sizeof(uint32) * store->nbuckets);
	for (uint32 row = 0; row < store->nrows; row++)
	{
		uint32		bucket = (store->links[row].hash >> store->hash_shift) &
			(store->nbuckets - 1);

		for (;;)
		{
			uint32		head = store->buckets[bucket];

			if (head == 0)
			{
				store->links[row].next = 0;
				store->buckets[bucket] = row + 1;
				break;
			}
			if (store->links[head - 1].hash == store->links[row].hash &&
				build_keys_equal(state, head - 1, row))
			{
				store->links[row].next = head;
				store->buckets[bucket] = row + 1;
				store->has_duplicates = true;
				break;
			}
			bucket = (bucket + 1) & (store->nbuckets - 1);
		}
	}
}

static void
build_hash_table(BatchHashJoinState *state)
{
	BuildStore *store = &state->build;
	uint64		estimated_rows;
	uint32		initial_capacity = 1024;

	hash_join_reset_build_store(store);
	store->hash_shift = 0;
	estimated_rows = (uint64) state->inner_plan->plan->plan_rows;
	if ((double) estimated_rows < state->inner_plan->plan->plan_rows)
		estimated_rows++;
	while (initial_capacity < estimated_rows &&
		   initial_capacity <= UINT32_MAX / 2)
		initial_capacity *= 2;
	if (state->planned_partitions == 1 &&
		hash_join_estimated_build_memory(initial_capacity, store->ncolumns) <=
		state->memory_limit)
		resize_build_store(store, initial_capacity);
	if (state->planned_partitions > 1)
		hash_join_begin_spill(state,
							 state->memory_limit * state->planned_partitions,
							 estimated_rows);
	for (;;)
	{
		PgBatchInputBatch input;
		PgBatchBridgeBatch *batch;
		InputColumn *columns;
		uint32		hashes[PG_BATCH_SIZE];
		uint64		hash_rows;

		if (!pg_batch_input_advance(state->inner_input, &input))
			break;
		batch = input.batch;
		state->build_batches++;
		state->build_input_rows += pg_batch_row_count(batch);
		columns = MemoryContextAllocZero(state->join_context,
										 sizeof(InputColumn) * state->ninner_columns);
		for (int column = 0; column < state->ninner_columns; column++)
			hash_join_load_input_column(batch,
									state->inner_batch_columns[column],
							  batch->selection, PG_BATCH_PROJECT_PHASE,
								  &columns[column], state->join_context);
		hash_rows = hash_input_batch(state, batch, columns,
								 state->inner_keys, hashes);
		if (!state->spilled &&
			hash_join_estimated_build_memory(
				store->nrows + pg_batch_row_count(batch),
								   store->ncolumns) > state->memory_limit)
		{
			uint64		estimated = hash_join_estimated_build_memory(
				store->nrows + pg_batch_row_count(batch), store->ncolumns);

			hash_join_track_peak_memory(state);
			hash_join_begin_spill(state, estimated,
				store->nrows + pg_batch_row_count(batch));
			state->build_rows += store->nrows;
			hash_join_spill_build_store(state);
			limit_resident_build(state);
		}
		if (state->spilled)
			state->build_rows += append_or_spill_build_batch(state, batch,
													 columns, hashes, hash_rows);
		else
			while (hash_rows != 0)
			{
				int			row = pg_rightmost_one_pos64(hash_rows);

				append_build_row_hash(state, columns, row, hashes[row]);
				hash_rows &= hash_rows - 1;
			}
		pfree(columns);
	}
	if (state->spilled)
	{
		if (state->build_rows > 0)
		{
			for (int partition = 0; partition < state->npartitions;
				 partition++)
			{
				if (bms_is_member(partition, state->resident_partitions) &&
					state->resident_partition_rows[partition] == 0)
					state->resident_partitions = bms_del_member(
						state->resident_partitions, partition);
			}
			hash_join_finish_spilled_build(state);
			state->bloom_usable = state->bloom_words != NULL &&
				state->bloom_nbits / state->build_rows >= 2;
			if (store->nrows > 0)
			{
				store->hash_shift = 0;
				hash_join_finish_build_table(state);
			}
		}
		else
		{
			state->current_partition = -1;
			state->partition_active = false;
		}
	}
	else
	{
		hash_join_finish_build_table(state);
		state->build_rows = store->nrows;
		hash_join_track_peak_memory(state);
	}
	state->built = true;
}

void
hash_join_prepare_probe_keys(BatchHashJoinState *state)
{
	Bitmapset  *columns = state->probe_from_spill ?
		state->spill_outer_key_columns :
		state->outer_key_columns;

	pg_batch_prepare_columns(state->probe_batch, columns,
							 state->probe_batch->selection,
							 PG_BATCH_FILTER_PHASE);
	for (int key = 0; key < state->nkeys; key++)
	{
		int			raw_column = state->outer_keys[key];
		int			batch_column = state->probe_from_spill ? raw_column :
			state->outer_batch_columns[raw_column];

		bind_input_column(state->probe_batch, batch_column,
						  state->probe_batch->selection,
						  PG_BATCH_FILTER_PHASE,
						  &state->probe_columns[raw_column]);
	}
}

static bool
fetch_probe_batch(BatchHashJoinState *state)
{
	if (state->spilled)
	{
		if (state->build_rows == 0)
			return false;
		if (state->spill_probe_started)
			return hash_join_fetch_spilled_probe_batch(state);
		while (!state->probe_input_done)
		{
			PgBatchInputBatch input;
			PgBatchBridgeBatch *batch;
			uint32		hashes[PG_BATCH_SIZE];
			uint64		hash_rows;

			if (hash_join_publish_resident_probe(state, false))
				return true;
			if (!pg_batch_input_advance(state->outer_input, &input))
			{
				hash_join_finish_spilled_probe(state);
				state->probe_input_done = true;
				continue;
			}
			batch = input.batch;
			state->probe_batches++;
			state->probe_rows_seen += pg_batch_row_count(batch);
			MemSet(state->probe_columns, 0,
				   sizeof(InputColumn) * state->nouter_columns);
			for (int key = 0; key < state->nkeys; key++)
			{
				int			column = state->outer_keys[key];

				hash_join_load_input_column(batch,
					state->outer_batch_columns[column], batch->selection,
					PG_BATCH_FILTER_PHASE, &state->probe_columns[column],
					state->join_context);
			}
			hash_rows = hash_input_batch(state, batch, state->probe_columns,
									 state->outer_keys, hashes);
			(void) hash_join_spill_input_batch(state, batch,
				state->probe_columns, state->nouter_columns,
				hashes, hash_rows, state->outer_batch_columns,
				state->probe_files, NULL);
		}
		if (hash_join_publish_resident_probe(state, true))
			return true;
		hash_join_track_peak_memory(state);
		hash_join_reset_build_store(&state->build);
		state->spill_probe_started = true;
		return hash_join_fetch_spilled_probe_batch(state);
	}
	for (;;)
	{
		PgBatchInputBatch input;

		if (!pg_batch_input_advance(state->outer_input, &input))
			return false;
		state->probe_batch = input.batch;
		state->probe_from_spill = false;
		state->probe_batches++;
		state->probe_rows_seen += pg_batch_row_count(state->probe_batch);
		MemSet(state->probe_columns, 0,
			   sizeof(InputColumn) * state->nouter_columns);
		state->next_probe_row = -1;
		hash_join_prepare_probe_keys(state);
		state->probe_hash_rows = hash_input_batch(state, state->probe_batch,
											 state->probe_columns,
											 state->outer_keys,
											 state->probe_hashes);
		return true;
	}
}

static void
finish_probe_batch(BatchHashJoinState *state)
{
	if (state->probe_batch == NULL)
		return;
	if (state->probe_from_resident)
		hash_join_finish_resident_probe(state);
	state->probe_batch = NULL;
}

static uint32
find_build_head(BatchHashJoinState *state, uint32 hash, int probe_row)
{
	BuildStore *store = &state->build;
	uint32		bucket = (hash >> store->hash_shift) & (store->nbuckets - 1);

	for (;;)
	{
		uint32		link = store->buckets[bucket];
		bool		matches = link != 0 &&
			store->links[link - 1].hash == hash;

		/* The combined hash proves equality of the final key; see above. */
		for (int key = 0; matches && key < state->nkeys - 1; key++)
		{
			int			inner_column = state->inner_keys[key];
			int			outer_column = state->outer_keys[key];
			bool		isnull;
			int32		probe_value = hash_join_input_value(
				&state->probe_columns[outer_column], probe_row, &isnull);

			Assert(!isnull);
			matches = store->columns[inner_column].values[link - 1] ==
				probe_value;
		}
		if (matches || link == 0)
			return link;
		bucket = (bucket + 1) & (store->nbuckets - 1);
	}
}

static bool
load_probe_hash(BatchHashJoinState *state, int row, uint32 *hash)
{
	if (state->probe_from_resident)
		*hash = state->resident_probe_block.hashes[row];
	else if (state->probe_from_spill)
		*hash = state->probe_block.hashes[row];
	else
	{
		if ((state->probe_hash_rows & (UINT64CONST(1) << row)) == 0)
			return false;
		*hash = state->probe_hashes[row];
	}
	return true;
}

static bool
load_probe_window(BatchHashJoinState *state)
{
	int			count = 0;
	int			row = state->next_probe_row;

	while (count < PG_BATCH_SIZE &&
		   (row = pg_batch_bridge_next_selected(state->probe_batch, row)) >= 0)
	{
		uint32		hash;

		state->next_probe_row = row;
		state->probe_rows[count] = row;
		if (load_probe_hash(state, row, &hash))
		{
			state->probe_build_rows[count] = find_build_head(state, hash, row);
			count++;
		}
	}
	state->probe_active = count;
	return count > 0;
}

/* Avoid a second pass over each probe window when every build key is unique. */
static bool
probe_unique_rows(BatchHashJoinState *state)
{
	int			row = state->next_probe_row;

	while (state->output_count < PG_BATCH_SIZE &&
		   (row = pg_batch_bridge_next_selected(state->probe_batch, row)) >= 0)
	{
		uint32		hash;
		uint32		link;

		state->next_probe_row = row;
		if (!load_probe_hash(state, row, &hash))
			continue;
		link = find_build_head(state, hash, row);
		if (link != 0)
		{
			int			output = state->output_count++;

			state->output_probe_rows[output] = row;
			state->output_build_rows[output] = link - 1;
		}
	}
	return state->output_count == PG_BATCH_SIZE;
}

static bool
build_value_valid(const BuildStore *store, int column, uint32 row)
{
	return (store->columns[column].validity[row / 64] &
			(UINT64CONST(1) << (row % 64))) != 0;
}

static void
ensure_probe_raw_column(BatchHashJoinState *state, int column, int row)
{
	ensure_input_row(state->probe_batch,
					 state->probe_from_spill ? column :
					 state->outer_batch_columns[column], row,
					 &state->probe_columns[column], PG_BATCH_PROJECT_PHASE,
					 state->join_context);
}

static void
fill_raw_slot(BatchHashJoinState *state, int probe_row, uint32 build_row,
			  const Bitmapset *needed)
{
	TupleTableSlot *slot = state->raw_slot;
	int			column = -1;

	ExecClearTuple(slot);
	MemSet(slot->tts_values, 0, sizeof(Datum) *
			state->raw_slot->tts_tupleDescriptor->natts);
	MemSet(slot->tts_isnull, true, sizeof(bool) *
			state->raw_slot->tts_tupleDescriptor->natts);
	while (needed != NULL && (column = bms_next_member(needed, column)) >= 0)
	{
		if (column < state->nouter_columns)
		{
			bool		isnull;
			int32		value;

			ensure_probe_raw_column(state, column, probe_row);
			value = hash_join_input_value(&state->probe_columns[column], probe_row,
							&isnull);
			slot->tts_isnull[column] = isnull;
			if (!isnull)
				slot->tts_values[column] = Int32GetDatum(value);
		}
		else
		{
			int			inner_column = column - state->nouter_columns;
			bool		isnull = !build_value_valid(&state->build,
												 inner_column, build_row);

			slot->tts_isnull[column] = isnull;
			if (!isnull)
				slot->tts_values[column] = Int32GetDatum(
					state->build.columns[inner_column].values[build_row]);
		}
	}
	ExecStoreVirtualTuple(slot);
}

static bool
passes_scalar_qual(BatchHashJoinState *state, int probe_row,
				   uint32 build_row)
{
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;

	if (state->css.ss.ps.qual == NULL)
		return true;
	ResetExprContext(econtext);
	fill_raw_slot(state, probe_row, build_row, state->qual_raw_columns);
	econtext->ecxt_scantuple = state->raw_slot;
	if (!ExecQual(state->css.ss.ps.qual, econtext))
		return false;
	return true;
}

static void
probe_round(BatchHashJoinState *state)
{
	BuildStore *store = &state->build;
	int			processed = 0;
	int			remaining = 0;

	for (int lane = 0; lane < state->probe_active; lane++)
	{
		uint32		link = state->probe_build_rows[lane];
		uint32		build_row;

		if (link == 0)
		{
			processed++;
			continue;
		}
		build_row = link - 1;
		{
			int			output = state->output_count++;

			state->output_probe_rows[output] = state->probe_rows[lane];
			state->output_build_rows[output] = build_row;
		}
		state->probe_build_rows[lane] = store->links[build_row].next;
		if (state->probe_build_rows[lane] != 0)
			remaining++;
		processed++;
		if (state->output_count == PG_BATCH_SIZE)
			break;
	}

	/* The common unique-key case finishes every lane in one round. */
	if (processed == state->probe_active && remaining == 0)
	{
		state->probe_active = 0;
		return;
	}

	{
		int			write = 0;

		for (int lane = 0; lane < state->probe_active; lane++)
		{
			if (lane >= processed || state->probe_build_rows[lane] != 0)
			{
				if (write != lane)
				{
					state->probe_rows[write] = state->probe_rows[lane];
					state->probe_build_rows[write] =
						state->probe_build_rows[lane];
				}
				write++;
			}
		}
		state->probe_active = write;
	}
}

static bool
raw_value(BatchHashJoinState *state, int raw_column, int output_row,
		  int32 *value, bool *isnull)
{
	int			probe_row = state->output_probe_rows[output_row];
	uint32		build_row = state->output_build_rows[output_row];

	if (raw_column < state->nouter_columns)
	{
		ensure_probe_raw_column(state, raw_column, probe_row);
		*value = hash_join_input_value(&state->probe_columns[raw_column], probe_row,
							 isnull);
	}
	else
	{
		int			inner_column = raw_column - state->nouter_columns;

		*isnull = !build_value_valid(&state->build, inner_column, build_row);
		*value = *isnull ? 0 :
			state->build.columns[inner_column].values[build_row];
	}
	return !*isnull;
}

static void
prepare_qual_columns(BatchHashJoinState *state, int first, int end)
{
	uint64		probe_rows = 0;
	int			raw_column = -1;

	if (state->qual_probe_columns == NULL)
		return;
	for (int row = first; row < end; row++)
		probe_rows |= UINT64CONST(1) << state->output_probe_rows[row];
	pg_batch_prepare_columns(state->probe_batch, state->qual_probe_columns,
							 &probe_rows, PG_BATCH_PROJECT_PHASE);
	while ((raw_column = bms_next_member(state->qual_raw_columns,
										   raw_column)) >= 0 &&
		   raw_column < state->nouter_columns)
	{
		int			batch_column = state->probe_from_spill ? raw_column :
			state->outer_batch_columns[raw_column];

		bind_input_column(state->probe_batch, batch_column, &probe_rows,
						  PG_BATCH_PROJECT_PHASE,
						  &state->probe_columns[raw_column]);
	}
}

/* The probe columns used here were bound for all candidate rows above. */
static inline int32
prepared_qual_value(BatchHashJoinState *state, int raw_column, int output_row,
					bool *isnull)
{
	if (raw_column < state->nouter_columns)
	{
		int			probe_row = state->output_probe_rows[output_row];

		return hash_join_input_value(&state->probe_columns[raw_column], probe_row,
						   isnull);
	}
	else
	{
		int			inner_column = raw_column - state->nouter_columns;
		uint32		build_row = state->output_build_rows[output_row];

		*isnull = !build_value_valid(&state->build, inner_column, build_row);
		return *isnull ? 0 :
			state->build.columns[inner_column].values[build_row];
	}
}

static inline bool
eval_builtin_int4_qual(Oid funcid, int32 left, int32 right)
{
	switch (funcid)
	{
		case F_INT4EQ:
			return left == right;
		case F_INT4NE:
			return left != right;
		case F_INT4LT:
			return left < right;
		case F_INT4LE:
			return left <= right;
		case F_INT4GT:
			return left > right;
		case F_INT4GE:
			return left >= right;
		default:
			elog(ERROR, "pg_batch found an invalid built-in int4 condition");
	}
}

static uint64
apply_batch_quals(BatchHashJoinState *state, uint64 selected)
{
	for (int q = 0; q < state->nbatch_quals && selected != 0; q++)
	{
		BatchJoinQual *qual = &state->batch_quals[q];
		MemoryContext oldcontext = NULL;
		bool		builtin = qual->fcinfo == NULL;
		bool		track_function = !builtin &&
			pgstat_track_functions > qual->fcinfo->flinfo->fn_stats;
		uint64		rows = selected;

		if (!builtin)
			oldcontext = MemoryContextSwitchTo(state->qual_context);
		while (rows != 0)
		{
			int			row = pg_rightmost_one_pos64(rows);
			uint64		bit = UINT64CONST(1) << row;
			bool		left_null;
			bool		right_null;
			int32		left;
			int32		right;
			bool		passes;
			PgStat_FunctionCallUsage usage;

			left = prepared_qual_value(state, qual->left_column, row,
									   &left_null);
			right = prepared_qual_value(state, qual->right_column, row,
										&right_null);
			if (left_null || right_null)
			{
				selected &= ~bit;
				rows &= ~bit;
				continue;
			}
			if (builtin)
				passes = eval_builtin_int4_qual(qual->funcid, left, right);
			else
			{
				Datum		result;

				qual->fcinfo->args[0].value = Int32GetDatum(left);
				qual->fcinfo->args[0].isnull = false;
				qual->fcinfo->args[1].value = Int32GetDatum(right);
				qual->fcinfo->args[1].isnull = false;
				qual->fcinfo->isnull = false;
				if (track_function)
					pgstat_init_function_usage(qual->fcinfo, &usage);
				result = FunctionCallInvoke(qual->fcinfo);
				if (track_function)
					pgstat_end_function_usage(&usage, true);
				passes = !qual->fcinfo->isnull && DatumGetBool(result);
			}
			if (!passes)
				selected &= ~bit;
			rows &= ~bit;
		}
		if (!builtin)
		{
			MemoryContextSwitchTo(oldcontext);
			MemoryContextReset(state->qual_context);
		}
	}
	return selected;
}

static void
qualify_output(BatchHashJoinState *state)
{
	int			first = state->output_qualified;
	int			end = state->output_count;
	uint64		before = first == 0 ? 0 : pg_batch_nrows_mask(first);
	uint64		selected = pg_batch_nrows_mask(end) & ~before;
	uint64		rows;
	int			write = first;

	if (first == end)
		return;
	if (state->nbatch_quals == 0 && state->css.ss.ps.qual == NULL)
	{
		state->output_qualified = end;
		return;
	}
	prepare_qual_columns(state, first, end);
	selected = apply_batch_quals(state, selected);
	rows = selected;
	while (rows != 0)
	{
		int			row = pg_rightmost_one_pos64(rows);
		uint64		bit = UINT64CONST(1) << row;

		if (!passes_scalar_qual(state, state->output_probe_rows[row],
								state->output_build_rows[row]))
			selected &= ~bit;
		rows &= ~bit;
	}
	for (int row = first; row < end; row++)
	{
		if ((selected & (UINT64CONST(1) << row)) == 0)
			continue;
		state->output_probe_rows[write] = state->output_probe_rows[row];
		state->output_build_rows[write] = state->output_build_rows[row];
		write++;
	}
	InstrCountFiltered1(&state->css.ss, end - first - (write - first));
	state->output_count = write;
	state->output_qualified = write;
}

static void
prepare_output_value(BatchHashJoinState *state, int column, int row)
{
	OutputColumn *output = &state->output_columns[column];
	uint64		bit = UINT64CONST(1) << row;
	int32		value = 0;
	bool		isnull;

	if ((output->native_rows & bit) != 0)
		return;
	if (output->direct_raw_column >= 0)
		raw_value(state, output->direct_raw_column, row, &value, &isnull);
	else
	{
		ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
		Datum		datum;

		ResetExprContext(econtext);
		fill_raw_slot(state, state->output_probe_rows[row],
					  state->output_build_rows[row], output->raw_columns);
		econtext->ecxt_scantuple = state->raw_slot;
		datum = ExecEvalExprSwitchContext(output->expr, econtext, &isnull);
		if (!isnull)
			value = DatumGetInt32(datum);
	}
	output->values[row] = value;
	if (!isnull)
		output->validity[row / 8] |= (uint8) 1 << (row % 8);
	else
		output->null_count++;
	output->native_rows |= bit;
}

static void
prepare_direct_output_column(BatchHashJoinState *state, OutputColumn *output,
							 uint64 rows)
{
	int			raw_column = output->direct_raw_column;

	while (rows != 0)
	{
		int			row = pg_rightmost_one_pos64(rows);
		uint64		bit = UINT64CONST(1) << row;
		bool		isnull;
		int32		value;

		if (raw_column < state->nouter_columns)
		{
			int			probe_row = state->output_probe_rows[row];
			InputColumn *input = &state->probe_columns[raw_column];

			if (!input->prepared ||
				!pg_batch_int4_row_available(&input->vector, probe_row))
				ensure_probe_raw_column(state, raw_column, probe_row);
			value = hash_join_input_value(input, probe_row, &isnull);
		}
		else
		{
			int			inner_column = raw_column - state->nouter_columns;
			uint32		build_row = state->output_build_rows[row];

			isnull = !build_value_valid(&state->build, inner_column,
										 build_row);
			value = isnull ? 0 :
				state->build.columns[inner_column].values[build_row];
		}
		output->values[row] = value;
		if (!isnull)
			output->validity[row / 8] |= (uint8) 1 << (row % 8);
		else
			output->null_count++;
		output->native_rows |= bit;
		rows &= ~bit;
	}
}

static void
prepare_output_columns(PgBatchBridgeBatch *batch,
					   const Bitmapset *columns, const uint64 *rows,
					   PgBatchBridgeMaterializePhase phase)
{
	BatchHashJoinState *state = batch->private_data;
	int			column = -1;
	uint64		selected = rows[0];

	while (columns != NULL &&
		   (column = bms_next_member(columns, column)) >= 0)
	{
		OutputColumn *output;
		uint64		missing;

		if (column >= state->noutput_columns)
			elog(ERROR, "pg_batch hash join output column is out of range");
		output = &state->output_columns[column];
		missing = selected & ~output->native_rows;
		if (output->direct_raw_column >= 0)
		{
			prepare_direct_output_column(state, output, missing);
			continue;
		}
		while (missing != 0)
		{
			int			row = pg_rightmost_one_pos64(missing);

			prepare_output_value(state, column, row);
			missing &= ~(UINT64CONST(1) << row);
		}
	}
}

static void
get_output_datum_column(PgBatchBridgeBatch *batch, int column,
						const uint64 *rows,
						PgBatchBridgeMaterializePhase phase,
						PgBatchBridgeDatumColumn *result)
{
	BatchHashJoinState *state = batch->private_data;
	OutputColumn *output;
	Bitmapset  *columns;
	uint64		missing;
	MemoryContext oldcontext;

	if (column < 0 || column >= state->noutput_columns)
		elog(ERROR, "pg_batch hash join output column is out of range");
	oldcontext = MemoryContextSwitchTo(
		pg_batch_slot_cast(state->output_slot)->batch_context);
	columns = bms_make_singleton(column);
	prepare_output_columns(batch, columns, rows, phase);
	bms_free(columns);
	MemoryContextSwitchTo(oldcontext);
	output = &state->output_columns[column];
	missing = rows[0] & ~output->datum_rows;
	while (missing != 0)
	{
		int			row = pg_rightmost_one_pos64(missing);
		uint64		bit = UINT64CONST(1) << row;
		bool		valid = (output->validity[row / 8] &
							  ((uint8) 1 << (row % 8))) != 0;

		output->isnull[row] = !valid;
		output->datums[row] = valid ? Int32GetDatum(output->values[row]) : 0;
		output->datum_rows |= bit;
		missing &= ~bit;
	}
	result->values = output->datums;
	result->isnull = output->isnull;
	result->valid_rows = &output->datum_rows;
	result->nwords = 1;
}

static void
get_output_arrow_column(PgBatchBridgeBatch *batch, int column,
						PgBatchArrowView *view)
{
	BatchHashJoinState *state = batch->private_data;
	OutputColumn *output = &state->output_columns[column];

	if ((output->native_rows & state->output_selection) !=
		state->output_selection)
		elog(ERROR, "pg_batch hash join output column has not been prepared");
	output->array.length = state->output_count;
	output->array.null_count = output->null_count;
	output->array.offset = 0;
	output->array.n_buffers = 2;
	output->array.buffers = output->buffers;
	output->schema.format = "i";
	output->schema.flags = ARROW_FLAG_NULLABLE;
	view->array = &output->array;
	view->schema = &output->schema;
}

static const PgBatchBridgeArrowInterface output_arrow_interface = {
	.abi_version = PG_BATCH_BRIDGE_ARROW_INTERFACE_VERSION,
	.struct_size = sizeof(PgBatchBridgeArrowInterface),
	.get_column = get_output_arrow_column,
};

static const void *
get_output_native_interface(PgBatchBridgeBatch *batch, const char *name,
							uint32 version)
{
	if (version == PG_BATCH_BRIDGE_ARROW_INTERFACE_VERSION &&
		strcmp(name, PG_BATCH_BRIDGE_ARROW_INTERFACE_NAME) == 0)
		return &output_arrow_interface;
	return NULL;
}

static const PgBatchBridgeBatchOps hash_join_batch_ops = {
	.abi_version = PG_BATCH_BRIDGE_ABI_VERSION,
	.struct_size = sizeof(PgBatchBridgeBatchOps),
	.prepare_columns = prepare_output_columns,
	.get_datum_column = get_output_datum_column,
	.get_native_interface = get_output_native_interface,
	.release = NULL,
};

static void
reset_output(BatchHashJoinState *state)
{
	for (int column = 0; column < state->noutput_columns; column++)
	{
		OutputColumn *output = &state->output_columns[column];

		MemSet(output->validity, 0, sizeof(output->validity));
		output->native_rows = 0;
		output->datum_rows = 0;
		output->null_count = 0;
		output->buffers[0] = output->validity;
		output->buffers[1] = output->values;
	}
	state->output_count = 0;
	state->output_qualified = 0;
	state->next_output_row = 0;
}

static TupleTableSlot *
publish_output(BatchHashJoinState *state)
{
	PgBatchSlot *slot = pg_batch_slot_cast(state->output_slot);

	state->output_selection = pg_batch_nrows_mask(state->output_count);
	state->output_batch.abi_version = PG_BATCH_BRIDGE_ABI_VERSION;
	state->output_batch.struct_size = sizeof(PgBatchBridgeBatch);
	state->output_batch.nrows = state->output_count;
	state->output_batch.nwords = 1;
	state->output_batch.selection = &state->output_selection;
	state->output_batch.table_oid = InvalidOid;
	state->output_batch.ops = &hash_join_batch_ops;
	state->output_batch.private_data = state;
	pg_batch_bridge->publish_batch(slot->binding, &state->output_batch);
	pg_batch_select_row(state->output_slot, 0);
	state->output_published = true;
	state->output_rows += state->output_count;
	if (state->output_request->return_batch &&
		state->css.ss.ps.instrument != NULL)
		state->css.ss.ps.instrument->tuplecount += state->output_count - 1;
	return state->output_slot;
}

static bool
produce_output(BatchHashJoinState *state)
{
	reset_output(state);
	for (;;)
	{
		if (state->probe_done_pending)
		{
			finish_probe_batch(state);
			state->probe_done_pending = false;
		}
		if (state->probe_active > 0)
		{
			probe_round(state);
			if (state->output_count == PG_BATCH_SIZE)
			{
				qualify_output(state);
				if (state->output_count == PG_BATCH_SIZE)
					return true;
			}
			continue;
		}
		if (state->probe_batch == NULL && !fetch_probe_batch(state))
			return state->output_count > 0;
		if (!state->build.has_duplicates)
		{
			if (probe_unique_rows(state))
			{
				qualify_output(state);
				if (state->output_count == PG_BATCH_SIZE)
					return true;
				continue;
			}
		}
		else if (load_probe_window(state))
			continue;

		if (state->output_count > 0)
		{
			qualify_output(state);
			if (state->output_count > 0)
			{
				state->probe_done_pending = true;
				return true;
			}
		}
		finish_probe_batch(state);
	}
}

static void
clear_published_output(BatchHashJoinState *state, bool require_consumed)
{
	if (!state->output_published)
		return;
	if (require_consumed && !state->output_batch.consumed)
		elog(ERROR, "pg_batch hash join parent requested a new batch too early");
	ExecClearTuple(state->output_slot);
	state->output_published = false;
}

static TupleTableSlot *
exec_batch(BatchHashJoinState *state)
{
	clear_published_output(state, true);
	if (!produce_output(state))
		return ExecClearTuple(state->output_slot);
	return publish_output(state);
}

static TupleTableSlot *
exec_row(BatchHashJoinState *state)
{
	for (;;)
	{
		if (!state->output_published)
		{
			if (!produce_output(state))
				return ExecClearTuple(state->output_slot);
			publish_output(state);
			state->next_output_row = 0;
		}
		if (state->next_output_row >= state->output_count)
		{
			state->output_batch.consumed = true;
			clear_published_output(state, false);
			continue;
		}
		pg_batch_select_row(state->output_slot, state->next_output_row++);
		return state->output_slot;
	}
}

static Bitmapset *
expression_raw_columns(Node *expr)
{
	Bitmapset  *result = NULL;
	List	   *vars = pull_var_clause(expr, PVC_RECURSE_AGGREGATES |
								 PVC_RECURSE_WINDOWFUNCS |
								 PVC_INCLUDE_PLACEHOLDERS);

	foreach_ptr(Var, var, vars)
	{
		if (var->varno != INDEX_VAR || var->varattno <= 0)
			elog(ERROR, "pg_batch hash join expression has an invalid variable");
		result = bms_add_member(result, var->varattno - 1);
	}
	list_free(vars);
	return result;
}

static void
init_output_columns(BatchHashJoinState *state, CustomScan *cscan)
{
	int			column = 0;

	state->noutput_columns = list_length(cscan->scan.plan.targetlist);
	state->output_columns = palloc0_array(OutputColumn,
										 state->noutput_columns);
	foreach_ptr(TargetEntry, tle, cscan->scan.plan.targetlist)
	{
		OutputColumn *output = &state->output_columns[column++];
		Node	   *expr = pg_batch_strip_relabel((Node *) tle->expr);

		output->direct_raw_column = -1;
		if (IsA(expr, Var) && castNode(Var, expr)->varno == INDEX_VAR)
			output->direct_raw_column = castNode(Var, expr)->varattno - 1;
		else
		{
			output->expr = ExecInitExpr((Expr *) tle->expr,
										&state->css.ss.ps);
			output->raw_columns = expression_raw_columns((Node *) tle->expr);
		}
	}
}

static void
configure_output_slot(BatchHashJoinState *state, EState *estate)
{
	List	   *logical_columns = NIL;
	Bitmapset  *survivors = NULL;
	TupleDesc	desc = state->css.ss.ps.ps_ResultTupleDesc;
	MemoryContext oldcontext;

	state->slot_context = AllocSetContextCreate(estate->es_query_cxt,
											"pg_batch hash join slot",
											ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(state->slot_context);
	state->output_slot = ExecInitExtraTupleSlot(estate, desc, &PgBatchSlotOps);
	MemoryContextSwitchTo(oldcontext);
	/*
	 * ExecCustomScan() returns output_slot, whose operations are PgBatchSlotOps.
	 * Do not advertise those operations as fixed, however: a scalar parent may
	 * use the metadata to allocate its own store slot and PgBatchSlotOps accepts
	 * only a published batch. Dynamic metadata makes expression steps inspect
	 * the actual returned slot while ordinary parent-owned slots stay virtual.
	 */
	state->css.ss.ps.resultops = &TTSOpsVirtual;
	state->css.ss.ps.resultopsfixed = false;
	state->css.ss.ps.resultopsset = true;
	state->css.ss.ps.ps_ProjInfo = NULL;
	for (int column = 0; column < state->noutput_columns; column++)
	{
		logical_columns = lappend_int(logical_columns, column + 1);
		survivors = bms_add_member(survivors, column);
	}
	pg_batch_configure_slot(pg_batch_slot_cast(state->output_slot), NULL,
							logical_columns, 0);
	pg_batch_set_request(state->output_slot, NULL, survivors, false);
	state->output_request = pg_batch_bridge->get_request(
		pg_batch_slot_cast(state->output_slot)->binding);
	list_free(logical_columns);
	bms_free(survivors);
}

static void
init_batch_quals(BatchHashJoinState *state, CustomScan *cscan)
{
	int			q = 0;

	state->nbatch_quals = list_length(cscan->custom_exprs);
	state->batch_quals = palloc0_array(BatchJoinQual,
									  state->nbatch_quals);
	foreach_ptr(OpExpr, op, cscan->custom_exprs)
	{
		BatchJoinQual *qual = &state->batch_quals[q++];
		Node	   *left = pg_batch_strip_relabel(linitial(op->args));
		Node	   *right = pg_batch_strip_relabel(lsecond(op->args));

		if (!IsA(left, Var) || !IsA(right, Var) ||
			castNode(Var, left)->varno != INDEX_VAR ||
			castNode(Var, right)->varno != INDEX_VAR)
			elog(ERROR, "pg_batch found an invalid batch join condition");
		qual->left_column = castNode(Var, left)->varattno - 1;
		qual->right_column = castNode(Var, right)->varattno - 1;
		qual->funcid = op->opfuncid;
		if (op->opfuncid != F_INT4EQ && op->opfuncid != F_INT4NE &&
			op->opfuncid != F_INT4LT && op->opfuncid != F_INT4LE &&
			op->opfuncid != F_INT4GT && op->opfuncid != F_INT4GE)
		{
			fmgr_info(op->opfuncid, &qual->func);
			fmgr_info_set_expr((Node *) op, &qual->func);
			qual->fcinfo = palloc0(SizeForFunctionCallInfo(2));
			InitFunctionCallInfoData(*qual->fcinfo, &qual->func, 2,
									 op->inputcollid, NULL, NULL);
		}
	}
}

static void
hash_join_begin(CustomScanState *node, EState *estate, int eflags)
{
	BatchHashJoinState *state = (BatchHashJoinState *) node;
	CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
	List	   *outer_columns = linitial(cscan->custom_private);
	List	   *inner_columns = lsecond(cscan->custom_private);
	List	   *outer_keys = lthird(cscan->custom_private);
	List	   *inner_keys = lfourth(cscan->custom_private);
	const char *outer_producer_name =
		strVal(list_nth(cscan->custom_private, 5));
	const char *inner_producer_name =
		strVal(list_nth(cscan->custom_private, 6));
	int			position = 0;

	state->nouter_columns = list_length(outer_columns);
	state->ninner_columns = list_length(inner_columns);
	state->nkeys = list_length(outer_keys);
	state->planned_partitions = intVal(list_nth(cscan->custom_private, 4));
	state->memory_limit = get_hash_memory_limit();
	state->raw_slot = node->ss.ss_ScanTupleSlot;
	state->join_context = AllocSetContextCreate(CurrentMemoryContext,
											"pg_batch hash join",
											ALLOCSET_DEFAULT_SIZES);
	state->build_context = AllocSetContextCreate(state->join_context,
											 "pg_batch hash build",
											 ALLOCSET_DEFAULT_SIZES);
	state->spill_context = AllocSetContextCreate(state->join_context,
											 "pg_batch hash spill",
											 ALLOCSET_DEFAULT_SIZES);
	state->qual_context = AllocSetContextCreate(state->join_context,
										  "pg_batch hash join quals",
										  ALLOCSET_START_SMALL_SIZES);
	state->build.context = state->build_context;
	state->build.ncolumns = state->ninner_columns;
	state->outer_batch_columns = palloc_array(int, state->nouter_columns);
	state->inner_batch_columns = palloc_array(int, state->ninner_columns);
	state->outer_keys = palloc_array(int, state->nkeys);
	state->inner_keys = palloc_array(int, state->nkeys);
	foreach_int(column, outer_columns)
		state->outer_batch_columns[position++] = column;
	position = 0;
	foreach_int(column, inner_columns)
		state->inner_batch_columns[position++] = column;
	position = 0;
	foreach_int(key, outer_keys)
		state->outer_keys[position++] = key;
	position = 0;
	foreach_int(key, inner_keys)
		state->inner_keys[position++] = key;
	pg_batch_init_children(node, estate, eflags);
	state->outer_plan = linitial(node->custom_ps);
	state->inner_plan = lsecond(node->custom_ps);
	state->outer_input = pg_batch_input_create(state->join_context,
		pg_batch_bridge, state->outer_plan, outer_producer_name);
	state->inner_input = pg_batch_input_create(state->join_context,
		pg_batch_bridge, state->inner_plan, inner_producer_name);
	for (int key = 0; key < state->nkeys; key++)
	{
		state->outer_key_columns = bms_add_member(state->outer_key_columns,
			state->outer_batch_columns[state->outer_keys[key]]);
		state->spill_outer_key_columns =
			bms_add_member(state->spill_outer_key_columns,
						   state->outer_keys[key]);
	}
	request_side_columns(pg_batch_input_request_binding(state->outer_input),
					 state->outer_batch_columns, state->nouter_columns,
					 state->outer_keys, state->nkeys);
	request_side_columns(pg_batch_input_request_binding(state->inner_input),
					 state->inner_batch_columns, state->ninner_columns,
					 state->inner_keys, state->nkeys);
	state->probe_columns = palloc0_array(InputColumn, state->nouter_columns);
	state->hash_keys = palloc_array(const PgBatchInt4Vector *, state->nkeys);
	state->qual_raw_columns = bms_union(
		expression_raw_columns((Node *) cscan->scan.plan.qual),
		expression_raw_columns((Node *) cscan->custom_exprs));
	{
		int			raw_column = -1;

		while ((raw_column = bms_next_member(state->qual_raw_columns,
											   raw_column)) >= 0 &&
			   raw_column < state->nouter_columns)
			state->qual_probe_columns = bms_add_member(
				state->qual_probe_columns,
				state->outer_batch_columns[raw_column]);
	}
	init_batch_quals(state, cscan);
	init_output_columns(state, cscan);
	configure_output_slot(state, estate);
	state->next_probe_row = -1;
}

static TupleTableSlot *
hash_join_exec(CustomScanState *node)
{
	BatchHashJoinState *state = (BatchHashJoinState *) node;

	if (!state->built)
		build_hash_table(state);
	if (state->output_request->return_batch)
		return exec_batch(state);
	return exec_row(state);
}

static void
hash_join_end(CustomScanState *node)
{
	BatchHashJoinState *state = (BatchHashJoinState *) node;

	clear_published_output(state, false);
	hash_join_close_spill_files(state);
	pg_batch_end_children(node);
	if (state->join_context != NULL)
		MemoryContextDelete(state->join_context);
}

static void
hash_join_rescan(CustomScanState *node)
{
	BatchHashJoinState *state = (BatchHashJoinState *) node;

	clear_published_output(state, false);
	if (state->probe_batch != NULL)
		finish_probe_batch(state);
	hash_join_close_spill_files(state);
	MemoryContextReset(state->spill_context);
	state->probe_batch = NULL;
	state->probe_active = 0;
	state->probe_done_pending = false;
	state->built = false;
	state->spilled = false;
	state->npartitions = 0;
	state->partition_shift = 0;
	state->resident_partitions = NULL;
	state->resident_partition_rows = NULL;
	state->resident_build_rows = 0;
	state->resident_probe_rows = 0;
	state->resident_memory_limit = 0;
	state->bloom_words = NULL;
	state->bloom_nbits = 0;
	state->bloom_bytes = 0;
	state->bloom_usable = false;
	state->bloom_rejected_rows = 0;
	state->probe_input_done = false;
	state->probe_from_spill = false;
	state->probe_from_resident = false;
	state->spill_probe_started = false;
	state->current_partition = -1;
	state->partition_active = false;
	state->build_files = NULL;
	state->probe_files = NULL;
	state->build_partition_rows = NULL;
	state->active_build_file = NULL;
	state->active_probe_file = NULL;
	state->repartition_build_files = NULL;
	state->repartition_probe_files = NULL;
	state->repartition_build_rows = NULL;
	state->nrepartitions = 0;
	state->current_repartition = -1;
	state->build_batches = 0;
	state->build_input_rows = 0;
	state->build_rows = 0;
	state->probe_batches = 0;
	state->probe_rows_seen = 0;
	state->output_rows = 0;
	state->spill_bytes = 0;
	state->spill_build_rows = 0;
	state->spill_probe_rows = 0;
	state->spill_probe_reads = 0;
	state->spill_repartitions = 0;
	state->spill_pages_written = 0;
	state->spill_pages_read = 0;
	state->peak_memory = 0;
	pg_batch_rescan_children(node);
	pg_batch_input_reset(state->outer_input);
	pg_batch_input_reset(state->inner_input);
}

static void
hash_join_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	BatchHashJoinState *state = (BatchHashJoinState *) node;

	ExplainPropertyInteger("Hash Buckets", NULL, state->build.nbuckets, es);
	ExplainPropertyInteger("Planned Partitions", NULL,
						   state->planned_partitions, es);
	if (state->spilled)
	{
		ExplainPropertyInteger("Spill Partitions", NULL,
								   state->npartitions, es);
		ExplainPropertyInteger("Resident Partitions", NULL,
								   bms_num_members(state->resident_partitions), es);
		ExplainPropertyInteger("Resident Build Rows", NULL,
								   state->resident_build_rows, es);
		ExplainPropertyInteger("Spill Bytes Written", "kB",
								   state->spill_bytes / 1024, es);
		ExplainPropertyInteger("Spill Pages Written", NULL,
								   state->spill_pages_written, es);
		ExplainPropertyBool("Bloom Filter", state->bloom_usable, es);
		if (state->bloom_words != NULL)
			ExplainPropertyInteger("Bloom Filter Memory", "kB",
								   state->bloom_bytes / 1024, es);
		ExplainPropertyInteger("Repartitioned Spill Partitions", NULL,
							   state->spill_repartitions, es);
	}
	if (es->analyze)
	{
		ExplainPropertyInteger("Build Batches", NULL, state->build_batches, es);
		ExplainPropertyInteger("Build Input Rows", NULL,
							   state->build_input_rows, es);
		ExplainPropertyInteger("Build Rows", NULL, state->build_rows, es);
		ExplainPropertyInteger("Probe Batches", NULL, state->probe_batches, es);
		ExplainPropertyInteger("Probe Rows", NULL, state->probe_rows_seen, es);
		ExplainPropertyInteger("Output Rows", NULL, state->output_rows, es);
		if (state->spilled)
		{
			ExplainPropertyInteger("Resident Probe Rows", NULL,
								   state->resident_probe_rows, es);
			ExplainPropertyInteger("Build Rows Written To Spill", NULL,
								   state->spill_build_rows, es);
			ExplainPropertyInteger("Probe Rows Written To Spill", NULL,
								   state->spill_probe_rows, es);
			ExplainPropertyInteger("Probe Rows Rejected By Bloom", NULL,
								   state->bloom_rejected_rows, es);
			ExplainPropertyInteger("Probe Rows Read From Spill", NULL,
								   state->spill_probe_reads, es);
			ExplainPropertyInteger("Spill Pages Read", NULL,
								   state->spill_pages_read, es);
		}
		ExplainPropertyInteger("Peak Memory Usage", "kB",
							   state->peak_memory / 1024, es);
	}
}
