#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "utils/memutils.h"

#include "hash_join_internal.h"

/*
 * PgBatchHashJoin policy over the reusable spill module. Initial partitioning
 * keeps selected partitions resident. Large disk partitions can use more hash
 * bits; data that remains skewed is processed by rescanning frozen probe tape.
 */

/*
 * Repartitioning writes both sides once more. For a few cached probe passes,
 * that extra I/O costs more than it saves.
 */
#define PG_BATCH_REPARTITION_MIN_PASSES 6

static void rewind_spill_file(HashSpillFile *file);
static inline uint64 spill_validity_word(const uint8 *validity);

static TupleDesc
spill_tuple_desc(int ncolumns)
{
	TupleDesc	desc = CreateTemplateTupleDesc(ncolumns);

	for (int column = 0; column < ncolumns; column++)
		TupleDescInitEntry(desc, column + 1, NULL, INT4OID, -1, 0);
	TupleDescFinalize(desc);
	return desc;
}

static PgBatchSpillSet *
spill_set_for_files(BatchHashJoinState *state, HashSpillFile **files)
{
	if (files == state->build_files)
		return state->build_spill;
	if (files == state->probe_files)
		return state->probe_spill;
	if (files == state->repartition_build_files)
		return state->repartition_build_spill;
	if (files == state->repartition_probe_files)
		return state->repartition_probe_spill;
	elog(ERROR, "unknown pg_batch hash join spill file set");
	pg_unreachable();
}

static void
record_spill_writes(BatchHashJoinState *state, PgBatchSpillSet *set)
{
	const PgBatchSpillStats *stats = pg_batch_spill_stats(set);

	state->spill_bytes += stats->bytes_written;
	state->spill_pages_written += stats->blocks_written;
}

static inline void
bloom_add(BatchHashJoinState *state, uint32 hash)
{
	/* One well-distributed hash probe keeps the all-match overhead small. */
	uint64		mask = state->bloom_nbits - 1;
	uint64		first = hash & mask;

	state->bloom_words[first / 64] |= UINT64CONST(1) << (first % 64);
}

static inline bool
bloom_may_contain(const BatchHashJoinState *state, uint32 hash)
{
	uint64		mask = state->bloom_nbits - 1;
	uint64		first = hash & mask;

	return (state->bloom_words[first / 64] &
			(UINT64CONST(1) << (first % 64))) != 0;
}

static void
spill_prepare_columns(PgBatch *batch, const Bitmapset *columns,
					  const uint64 *rows,
					  PgBatchColumnPhase phase)
{
	/* Spill blocks are already stored as dense native int32 columns. */
}

static void
spill_get_arrow_column(PgBatch *batch, int column,
					   PgBatchArrowView *view)
{
	SpillBlock *block = batch->private_data;

	if (column < 0 || column >= block->ncolumns)
		elog(ERROR, "pg_batch spill column is out of range");
	view->array = &block->arrays[column];
	view->schema = &block->schemas[column];
}

static const PgBatchArrowInterface spill_arrow_interface = {
	.abi_version = PG_BATCH_ARROW_INTERFACE_VERSION,
	.struct_size = sizeof(PgBatchArrowInterface),
	.get_column = spill_get_arrow_column,
};

static const void *
spill_get_native_interface(PgBatch *batch, const char *name,
						   uint32 version)
{
	if (version == PG_BATCH_ARROW_INTERFACE_VERSION &&
		strcmp(name, PG_BATCH_ARROW_INTERFACE_NAME) == 0)
		return &spill_arrow_interface;
	return NULL;
}

static const PgBatchOps spill_batch_ops = {
	.abi_version = PG_BATCH_ABI_VERSION,
	.struct_size = sizeof(PgBatchOps),
	.prepare_columns = spill_prepare_columns,
	.get_datum_column = NULL,
	.get_native_interface = spill_get_native_interface,
	.release = NULL,
};

static void
init_spill_block(SpillBlock *block, int ncolumns, int capacity,
				 MemoryContext context, bool allocate_data)
{
	int			validity_bytes = (capacity + 7) / 8;

	block->ncolumns = ncolumns;
	block->capacity = capacity;
	if (allocate_data)
	{
		block->hashes = MemoryContextAlloc(context,
										 sizeof(uint32) * capacity);
		block->values = MemoryContextAlloc(context,
									   sizeof(int32) * ncolumns * capacity);
		block->validity = MemoryContextAlloc(context,
										 sizeof(uint8) * ncolumns *
										 validity_bytes);
	}
	block->buffers = MemoryContextAlloc(context,
										sizeof(void *) * ncolumns * 2);
	block->arrays = MemoryContextAllocZero(context,
										   sizeof(struct ArrowArray) * ncolumns);
	block->schemas = MemoryContextAllocZero(context,
											sizeof(struct ArrowSchema) * ncolumns);
	for (int column = 0; column < ncolumns; column++)
	{
		if (allocate_data)
		{
			block->buffers[column * 2] =
				&block->validity[column * validity_bytes];
			block->buffers[column * 2 + 1] =
				&block->values[column * capacity];
		}
		block->arrays[column].n_buffers = 2;
		block->arrays[column].buffers = &block->buffers[column * 2];
		block->schemas[column].format = "i";
		block->schemas[column].flags = ARROW_FLAG_NULLABLE;
	}
	block->batch.abi_version = PG_BATCH_ABI_VERSION;
	block->batch.struct_size = sizeof(PgBatch);
	block->batch.nwords = 1;
	block->batch.selection = &block->selection;
	block->batch.table_oid = InvalidOid;
	block->batch.ops = &spill_batch_ops;
	block->batch.private_data = block;
}

static bool
read_spill_block(BatchHashJoinState *state, HashSpillFile *file,
				 SpillBlock *block)
{
	PgBatchSpillBlock input;
	uint64		row_mask;

	if (file->reader == NULL)
		file->reader = pg_batch_spill_reader_open(file->set, file->partition);
	if (!pg_batch_spill_reader_next(file->reader, &input))
		return false;
	if (input.ncolumns != block->ncolumns ||
		block->capacity < PG_BATCH_SPILL_BLOCK_ROWS)
		elog(ERROR, "invalid pg_batch hash join spill block");
	/* Borrow the reader's dense block until its next read or rewind. */
	block->hashes = (uint32 *) input.hashes;
	block->values = (int32 *) input.values;
	block->validity = (uint8 *) input.validity;
	block->nrows = input.nrows;
	row_mask = pg_batch_nrows_mask(block->nrows);
	state->spill_pages_read++;
	for (int column = 0; column < block->ncolumns; column++)
	{
		struct ArrowArray *array = &block->arrays[column];

		block->buffers[column * 2] = &input.validity[column * 8];
		block->buffers[column * 2 + 1] =
			&input.values[column * PG_BATCH_SPILL_BLOCK_ROWS];
		array->length = block->nrows;
		array->offset = 0;
		array->null_count = block->nrows -
			pg_popcount64(spill_validity_word(&input.validity[column * 8]) &
				row_mask);
	}
	block->selection = row_mask;
	block->batch.nrows = block->nrows;
	return true;
}

static HashSpillFile *
spill_file(PgBatchSpillSet *set, HashSpillFile **file, int partition)
{
	if (*file == NULL)
	{
		*file = palloc0_object(HashSpillFile);
		(*file)->set = set;
		(*file)->partition = partition;
	}
	return *file;
}

static inline uint64
spill_validity_word(const uint8 *validity)
{
	uint64		result = 0;

	for (int byte = 0; byte < 8; byte++)
		result |= (uint64) validity[byte] << (byte * 8);
	return result;
}

static void
write_partition_rows(BatchHashJoinState *state, HashSpillFile **files,
					 int partition, InputColumn *columns, int ncolumns,
					 const int *rows, const uint32 *hashes, int nrows)
{
	PgBatchSpillSet *set = spill_set_for_files(state, files);
	uint64		selection = 0;

	for (int column = 0; column < ncolumns; column++)
		state->spill_input_vectors[column] = columns[column].vector;
	for (int i = 0; i < nrows; i++)
		selection |= UINT64CONST(1) << rows[i];
	pg_batch_spill_write(set, partition, state->spill_input_vectors,
		PG_BATCH_SIZE, &selection, hashes);
	spill_file(set, &files[partition], partition);
}

static void
buffer_resident_probe_row(BatchHashJoinState *state, InputColumn *columns,
						  int row, uint32 hash)
{
	SpillBlock *block = &state->resident_probe_block;
	int			output = block->nrows++;
	int			stride = (block->capacity + 7) / 8;

	if (output >= block->capacity)
		elog(ERROR, "pg_batch resident probe buffer overflow");
	block->hashes[output] = hash;
	for (int column = 0; column < block->ncolumns; column++)
	{
		bool		isnull;
		int32		value = hash_join_input_value(&columns[column], row, &isnull);

		block->values[column * block->capacity + output] = value;
		if (!isnull)
			block->validity[column * stride + output / 8] |=
				(uint8) 1 << (output % 8);
	}
	state->resident_probe_rows++;
}

uint64
hash_join_spill_input_batch(BatchHashJoinState *state, PgBatch *batch,
							InputColumn *columns, int ncolumns,
							const uint32 *hashes, uint64 hash_rows,
							const int *batch_columns, HashSpillFile **files,
							uint64 *resident_rows)
{
	int		   *next_rows;
	int		   *used_partitions;
	int			nused = 0;
	uint64		routed_rows = hash_rows;
	uint64		accepted = pg_popcount64(hash_rows);

	Assert(batch->nrows <= PG_BATCH_SIZE);
	Assert(batch->nwords == 1);
	next_rows = MemoryContextAlloc(state->join_context,
								   sizeof(int) * batch->nrows);
	used_partitions = MemoryContextAlloc(state->join_context,
									 sizeof(int) * batch->nrows);
	if (resident_rows != NULL)
		*resident_rows = 0;
	/* Apply routing filters to the rows already hashed by the caller. */
	for (uint64 rows = hash_rows; rows != 0; rows &= rows - 1)
	{
		int			row = pg_rightmost_one_pos64(rows);
		uint64		bit = UINT64CONST(1) << row;

		if (files == state->build_files && state->bloom_words != NULL)
			bloom_add(state, hashes[row]);
		else if (files == state->probe_files && state->bloom_usable &&
				 !bloom_may_contain(state, hashes[row]))
		{
			routed_rows &= ~bit;
			state->bloom_rejected_rows++;
		}
		else if (files == state->probe_files)
		{
			int			partition = hashes[row] &
				(state->npartitions - 1);

			if (!bms_is_member(partition, state->resident_partitions) &&
				state->build_files[partition] == NULL)
				routed_rows &= ~bit;
		}
	}
	if (files == state->probe_files)
	{
		uint64		survivors = routed_rows;

		/* Do not materialize probe payload rejected by the complete key filter. */
		for (int column = 0; column < ncolumns && survivors != 0; column++)
		{
			if (!columns[column].prepared)
				hash_join_load_input_column(batch, batch_columns[column],
					&survivors, PG_BATCH_PROJECT_PHASE, &columns[column],
					state->join_context);
		}
	}
	/* Link rows by partition without rescanning the batch per partition. */
	while (routed_rows != 0)
	{
		int			row = pg_rightmost_one_pos64(routed_rows);
		uint64		bit = UINT64CONST(1) << row;
		int			partition;

		partition = hashes[row] & (state->npartitions - 1);
		if (bms_is_member(partition, state->resident_partitions))
		{
			if (files == state->probe_files)
				buffer_resident_probe_row(state, columns, row, hashes[row]);
			else if (resident_rows != NULL)
				*resident_rows |= UINT64CONST(1) << row;
			routed_rows &= ~bit;
			continue;
		}
		if (files == state->build_files)
		{
			state->build_partition_rows[partition]++;
			state->spill_build_rows++;
		}
		if (files == state->probe_files &&
			state->build_files[partition] == NULL)
		{
			routed_rows &= ~bit;
			continue;
		}
		if (files == state->probe_files)
			state->spill_probe_rows++;
		if (state->spill_partition_heads[partition] < 0)
		{
			state->spill_partition_heads[partition] = row;
			used_partitions[nused++] = partition;
		}
		else
			next_rows[state->spill_partition_tails[partition]] = row;
		state->spill_partition_tails[partition] = row;
		next_rows[row] = -1;
		routed_rows &= ~bit;
	}
	for (int i = 0; i < nused; i++)
	{
		int			partition = used_partitions[i];
		int			rows[PG_BATCH_SIZE];
		int			count = 0;

		for (int row = state->spill_partition_heads[partition]; row >= 0;
			 row = next_rows[row])
		{
			rows[count++] = row;
			if (count == PG_BATCH_SIZE)
			{
				write_partition_rows(state, files, partition, columns,
									 ncolumns, rows, hashes, count);
				count = 0;
			}
		}
		if (count > 0)
			write_partition_rows(state, files, partition, columns, ncolumns,
								 rows, hashes, count);
		state->spill_partition_heads[partition] = -1;
	}
	pfree(used_partitions);
	pfree(next_rows);
	return accepted;
}

static void
write_store_block(BatchHashJoinState *state, HashSpillFile *file,
				  const int *rows, int nrows)
{
	BuildStore *store = &state->build;
	uint32		hashes[PG_BATCH_SIZE];
	int32	   *values = palloc_array(int32, store->ncolumns * nrows);
	uint64	   *validity = palloc0_array(uint64, store->ncolumns);

	for (int i = 0; i < nrows; i++)
		hashes[i] = store->links[rows[i]].hash;
	for (int column = 0; column < store->ncolumns; column++)
	{
		for (int i = 0; i < nrows; i++)
		{
			uint32		row = rows[i];

			values[column * nrows + i] = store->columns[column].values[row];
			if ((store->columns[column].validity[row / 64] &
				 (UINT64CONST(1) << (row % 64))) != 0)
				validity[column] |= UINT64CONST(1) << i;
		}
	}
	pg_batch_spill_write_dense(file->set, file->partition, nrows,
		hashes, values, nrows, validity);
	pfree(validity);
	pfree(values);
}

void
hash_join_spill_build_store(BatchHashJoinState *state)
{
	BuildStore *store = &state->build;
	uint32		write = 0;

	MemSet(state->resident_partition_rows, 0,
		   sizeof(uint64) * state->npartitions);

	for (int partition = 0; partition < state->npartitions; partition++)
	{
		int			rows[PG_BATCH_SIZE];
		int			count = 0;

		if (bms_is_member(partition, state->resident_partitions))
			continue;
		for (uint32 row = 0; row < store->nrows; row++)
		{
			if ((int) (store->links[row].hash & (state->npartitions - 1)) !=
				partition)
				continue;
			state->build_partition_rows[partition]++;
			state->spill_build_rows++;
			rows[count++] = row;
			if (count == PG_BATCH_SIZE)
			{
				write_store_block(state,
					spill_file(state->build_spill,
						&state->build_files[partition], partition),
								  rows, count);
				count = 0;
			}
		}
		if (count > 0)
			write_store_block(state,
				spill_file(state->build_spill,
					&state->build_files[partition], partition),
							  rows, count);
	}

	/* Compact rows belonging to the partitions that remain in memory. */
	for (uint32 row = 0; row < store->nrows; row++)
	{
		int			partition = store->links[row].hash &
			(state->npartitions - 1);

		if (!bms_is_member(partition, state->resident_partitions))
			continue;
		state->resident_partition_rows[partition]++;
		if (write != row)
		{
			store->links[write].hash = store->links[row].hash;
			for (int column = 0; column < store->ncolumns; column++)
			{
				bool		valid = (store->columns[column].validity[row / 64] &
					(UINT64CONST(1) << (row % 64))) != 0;

				store->columns[column].values[write] =
					store->columns[column].values[row];
				if (valid)
					store->columns[column].validity[write / 64] |=
						UINT64CONST(1) << (write % 64);
				else
					store->columns[column].validity[write / 64] &=
						~(UINT64CONST(1) << (write % 64));
			}
		}
		write++;
	}
	store->nrows = write;
	state->resident_build_rows = write;
}

static void
write_spill_rows(BatchHashJoinState *state, HashSpillFile **files,
				 int partition,
				 SpillBlock *block, int ncolumns, const int *rows, int nrows)
{
	uint32		hashes[PG_BATCH_SIZE];
	int32	   *values = palloc_array(int32, ncolumns * nrows);
	uint64	   *validity = palloc0_array(uint64, ncolumns);
	PgBatchSpillSet *set = spill_set_for_files(state, files);

	for (int i = 0; i < nrows; i++)
		hashes[i] = block->hashes[rows[i]];
	for (int column = 0; column < ncolumns; column++)
	{
		for (int i = 0; i < nrows; i++)
		{
			int			row = rows[i];

			values[column * nrows + i] =
				hash_join_spill_value(block, column, row);
			if (hash_join_spill_value_valid(block, column, row))
				validity[column] |= UINT64CONST(1) << i;
		}
	}
	pg_batch_spill_write_dense(set, partition, nrows, hashes, values, nrows,
		validity);
	spill_file(set, &files[partition], partition);
	pfree(validity);
	pfree(values);
}

static void
repartition_file(BatchHashJoinState *state, HashSpillFile *source,
				 SpillBlock *block, int ncolumns, HashSpillFile **files,
				 HashSpillFile **matching_build_files, uint64 *build_rows,
				 int hash_shift, int npartitions)
{
	int		   *heads = palloc_array(int, npartitions);
	int		   *tails = palloc_array(int, npartitions);

	for (int partition = 0; partition < npartitions; partition++)
		heads[partition] = -1;
	rewind_spill_file(source);
	block->nrows = 0;
	while (read_spill_block(state, source, block))
	{
		int			next_rows[PG_BATCH_SIZE];
		int			used_partitions[PG_BATCH_SIZE];
		int			nused = 0;

		for (int row = 0; row < block->nrows; row++)
		{
			int			partition =
				(block->hashes[row] >> hash_shift) & (npartitions - 1);

			if (matching_build_files != NULL &&
				matching_build_files[partition] == NULL)
				continue;
			if (heads[partition] < 0)
			{
				heads[partition] = row;
				used_partitions[nused++] = partition;
			}
			else
				next_rows[tails[partition]] = row;
			tails[partition] = row;
			next_rows[row] = -1;
		}
		for (int i = 0; i < nused; i++)
		{
			int			partition = used_partitions[i];
			int			rows[PG_BATCH_SIZE];
			int			nrows = 0;

			for (int row = heads[partition]; row >= 0; row = next_rows[row])
				rows[nrows++] = row;
			write_spill_rows(state, files, partition, block, ncolumns,
							 rows, nrows);
			if (build_rows != NULL)
				build_rows[partition] += nrows;
			heads[partition] = -1;
		}
	}
	pfree(tails);
	pfree(heads);
}

uint64
hash_join_estimated_build_memory(uint64 nrows, int ncolumns)
{
	uint64		capacity = 1024;

	while (capacity < nrows && capacity <= UINT32_MAX / 2)
		capacity *= 2;
	return capacity * (sizeof(uint32) * 2 + sizeof(int32) * ncolumns) +
		((capacity + 63) / 64) * sizeof(uint64) * ncolumns +
		capacity * sizeof(uint32) * 2;
}

static int
next_partition_count(uint64 required)
{
	int			result = 2;

	while ((uint64) result < required && result < 1024)
		result *= 2;
	return result;
}

void
hash_join_begin_spill(BatchHashJoinState *state, uint64 estimated_bytes,
					  uint64 estimated_rows)
{
	uint64		required = (estimated_bytes + state->memory_limit - 1) /
		state->memory_limit;
	TupleDesc	build_desc;
	TupleDesc	probe_desc;

	state->bloom_words = NULL;
	state->bloom_nbits = 0;
	state->bloom_bytes = 0;
	state->bloom_usable = false;
	state->npartitions = next_partition_count(Max(required,
												  (uint64) state->planned_partitions));
	state->partition_shift = 0;
	for (int partitions = state->npartitions; partitions > 1; partitions >>= 1)
		state->partition_shift++;
	state->build_files = MemoryContextAllocZero(state->spill_context,
											sizeof(HashSpillFile *) * state->npartitions);
	state->probe_files = MemoryContextAllocZero(state->spill_context,
											sizeof(HashSpillFile *) * state->npartitions);
	build_desc = spill_tuple_desc(state->ninner_columns);
	probe_desc = spill_tuple_desc(state->nouter_columns);
	state->build_spill = pg_batch_spill_create(state->spill_context,
		build_desc, state->ninner_columns, state->npartitions,
		state->memory_limit / 4);
	state->probe_spill = pg_batch_spill_create(state->spill_context,
		probe_desc, state->nouter_columns, state->npartitions,
		state->memory_limit / 4);
	FreeTupleDesc(build_desc);
	FreeTupleDesc(probe_desc);
	state->build_partition_rows = MemoryContextAllocZero(state->spill_context,
													 sizeof(uint64) * state->npartitions);
	state->resident_partition_rows = MemoryContextAllocZero(
		state->spill_context, sizeof(uint64) * state->npartitions);
	state->spill_partition_heads = MemoryContextAlloc(state->spill_context,
													  sizeof(int) * state->npartitions);
	state->spill_partition_tails = MemoryContextAlloc(state->spill_context,
													  sizeof(int) * state->npartitions);
	state->spill_input_vectors = MemoryContextAlloc(state->spill_context,
		sizeof(PgBatchInt4Vector) * Max(state->ninner_columns,
			state->nouter_columns));
	for (int partition = 0; partition < state->npartitions; partition++)
		state->spill_partition_heads[partition] = -1;
	{
		uint64		desired_bits = Max((uint64) 64, estimated_rows * 8);
		uint64		max_bits = Max((uint64) 64,
								(state->memory_limit / 16) * 8);
		uint64		nbits = 64;

		while (nbits < desired_bits && nbits <= max_bits / 2)
			nbits <<= 1;
		if (nbits > max_bits)
			nbits >>= 1;
		if (estimated_rows > 0 && nbits / estimated_rows >= 2)
		{
			state->bloom_nbits = nbits;
			state->bloom_bytes = nbits / 8;
			state->bloom_words = MemoryContextAllocZero(state->spill_context,
															 state->bloom_bytes);
			for (uint32 row = 0; row < state->build.nrows; row++)
				bloom_add(state, state->build.links[row].hash);
		}
	}
	{
		uint64		columns = Max(state->ninner_columns,
								  state->nouter_columns);
		uint64		bytes = sizeof(int) * state->npartitions +
			sizeof(uint32) * state->npartitions * PG_BATCH_SIZE +
			(sizeof(int32) * PG_BATCH_SIZE + sizeof(uint64)) *
			state->npartitions * columns;

		/* Leave most of the hash memory limit for the active build chunk. */
		state->resident_memory_limit = state->memory_limit -
			Min(state->memory_limit, Min(bytes, state->memory_limit / 4) +
				state->bloom_bytes);
	}
	{
		uint64		per_partition = Max((uint64) 1,
									 estimated_bytes / state->npartitions);
		int			nresident = Min(state->npartitions - 1,
			(int) (state->resident_memory_limit / per_partition));

		/* A normally sized partition should fit even after power-of-two rounding. */
		if (nresident == 0)
			nresident = 1;
		for (int partition = 0; partition < nresident; partition++)
			state->resident_partitions =
				bms_add_member(state->resident_partitions, partition);
	}
	init_spill_block(&state->build_block, state->ninner_columns,
					 PG_BATCH_SIZE,
					 state->spill_context, false);
	init_spill_block(&state->probe_block, state->nouter_columns,
					 PG_BATCH_SIZE,
					 state->spill_context, false);
	init_spill_block(&state->resident_probe_block, state->nouter_columns,
					 PG_BATCH_SIZE * 2, state->spill_context, true);
	MemSet(state->resident_probe_block.validity, 0,
		   sizeof(uint8) * state->nouter_columns *
		   ((state->resident_probe_block.capacity + 7) / 8));
	state->build_chunk_rows = Max(64,
								  (uint32) Min((uint64) UINT32_MAX,
											   (state->memory_limit / 2) /
											   Max((uint64) 1,
												   hash_join_estimated_build_memory(1024, state->ninner_columns) /
												   1024)));
	hash_join_track_peak_memory(state);
	state->spilled = true;
}

static void
append_spill_build_row(BatchHashJoinState *state, SpillBlock *block, int row)
{
	BuildStore *store = &state->build;
	uint32		stored_row;

	if (store->nrows == store->capacity)
		hash_join_grow_build_store(store);
	stored_row = store->nrows++;
	store->links[stored_row].hash = block->hashes[row];
	for (int column = 0; column < store->ncolumns; column++)
	{
		store->columns[column].values[stored_row] =
			hash_join_spill_value(block, column, row);
		if (hash_join_spill_value_valid(block, column, row))
			store->columns[column].validity[stored_row / 64] |=
				UINT64CONST(1) << (stored_row % 64);
	}
}

static bool
load_build_chunk(BatchHashJoinState *state)
{
	HashSpillFile *file = state->active_build_file;

	hash_join_reset_build_store(&state->build);
	state->build.hash_shift = state->active_hash_shift;
	while (state->build.nrows < state->build_chunk_rows)
	{
		if (state->build_block_row >= state->build_block.nrows)
		{
			if (!read_spill_block(state, file, &state->build_block))
			{
				state->build_file_eof = true;
				break;
			}
			state->build_block_row = 0;
		}
		while (state->build_block_row < state->build_block.nrows &&
			   state->build.nrows < state->build_chunk_rows)
			append_spill_build_row(state, &state->build_block,
								   state->build_block_row++);
	}
	if (state->build.nrows == 0)
	{
		state->build_partition_more = false;
		return false;
	}
	hash_join_finish_build_table(state);
	state->build_partition_more = !state->build_file_eof ||
		state->build_block_row < state->build_block.nrows;
	hash_join_track_peak_memory(state);
	return true;
}

static void
rewind_spill_file(HashSpillFile *file)
{
	/* The first reader starts at the beginning when it is opened. */
	if (file->reader != NULL)
		pg_batch_spill_reader_rewind(file->reader);
}

static void
finish_active_partition(BatchHashJoinState *state)
{
	if (state->active_build_file != NULL)
	{
		pg_batch_spill_reader_close(state->active_build_file->reader);
		state->active_build_file->reader = NULL;
	}
	if (state->active_probe_file != NULL)
	{
		pg_batch_spill_reader_close(state->active_probe_file->reader);
		state->active_probe_file->reader = NULL;
	}
	state->active_build_file = NULL;
	state->active_probe_file = NULL;
}

static int
partition_bits(int npartitions)
{
	int			bits = 0;

	for (; npartitions > 1; npartitions >>= 1)
		bits++;
	return bits;
}

static bool
start_partition_files(BatchHashJoinState *state, HashSpillFile *build_file,
					  HashSpillFile *probe_file, int hash_shift)
{
	state->active_build_file = build_file;
	state->active_probe_file = probe_file;
	state->active_hash_shift = hash_shift;
	rewind_spill_file(build_file);
	state->build_block.nrows = 0;
	state->build_block_row = 0;
	state->build_file_eof = false;
	if (!load_build_chunk(state))
		return false;
	rewind_spill_file(probe_file);
	state->partition_active = true;
	return true;
}

static bool
repartition_current_partition(BatchHashJoinState *state)
{
	uint64		rows = state->build_partition_rows[state->current_partition];
	uint64		required = (rows + state->build_chunk_rows - 1) /
		state->build_chunk_rows;
	int			npartitions;
	HashSpillFile *build_file = state->build_files[state->current_partition];
	HashSpillFile *probe_file = state->probe_files[state->current_partition];
	TupleDesc	build_desc;
	TupleDesc	probe_desc;

	/*
	 * Below this point another full write costs more than cached probe
	 * passes.
	 */
	if (required < PG_BATCH_REPARTITION_MIN_PASSES)
		return false;
	npartitions = next_partition_count(required);
	state->repartition_build_files = MemoryContextAllocZero(
		state->spill_context, sizeof(HashSpillFile *) * npartitions);
	state->repartition_probe_files = MemoryContextAllocZero(
		state->spill_context, sizeof(HashSpillFile *) * npartitions);
	state->repartition_build_rows = MemoryContextAllocZero(
													   state->spill_context, sizeof(uint64) * npartitions);
	build_desc = spill_tuple_desc(state->ninner_columns);
	probe_desc = spill_tuple_desc(state->nouter_columns);
	state->repartition_build_spill = pg_batch_spill_create(
		state->spill_context, build_desc, state->ninner_columns, npartitions,
		state->memory_limit / 4);
	state->repartition_probe_spill = pg_batch_spill_create(
		state->spill_context, probe_desc, state->nouter_columns, npartitions,
		state->memory_limit / 4);
	FreeTupleDesc(build_desc);
	FreeTupleDesc(probe_desc);
	state->nrepartitions = npartitions;
	state->current_repartition = -1;
	state->repartition_hash_shift = state->partition_shift +
		partition_bits(npartitions);
	state->spill_repartitions++;
	repartition_file(state, build_file, &state->build_block,
					 state->ninner_columns, state->repartition_build_files,
					 NULL, state->repartition_build_rows,
					 state->partition_shift, npartitions);
	repartition_file(state, probe_file, &state->probe_block,
					 state->nouter_columns, state->repartition_probe_files,
					 state->repartition_build_files, NULL,
					 state->partition_shift, npartitions);
	pg_batch_spill_finish(state->repartition_build_spill, false);
	pg_batch_spill_finish(state->repartition_probe_spill, true);
	record_spill_writes(state, state->repartition_build_spill);
	record_spill_writes(state, state->repartition_probe_spill);
	pg_batch_spill_reader_close(build_file->reader);
	pg_batch_spill_reader_close(probe_file->reader);
	build_file->reader = NULL;
	probe_file->reader = NULL;
	state->build_files[state->current_partition] = NULL;
	state->probe_files[state->current_partition] = NULL;
	return true;
}

static void
finish_repartition(BatchHashJoinState *state)
{
	if (state->current_repartition >= 0)
	{
		int			partition = state->current_repartition;

		state->repartition_build_files[partition] = NULL;
		state->repartition_probe_files[partition] = NULL;
	}
}

static bool
start_next_partition(BatchHashJoinState *state)
{
	finish_active_partition(state);
	for (;;)
	{
		if (state->repartition_build_files != NULL)
		{
			finish_repartition(state);
			while (++state->current_repartition < state->nrepartitions)
			{
				int			partition = state->current_repartition;

				if (state->repartition_build_files[partition] == NULL ||
					state->repartition_probe_files[partition] == NULL)
				{
					finish_repartition(state);
					continue;
				}
				if (start_partition_files(state,
										  state->repartition_build_files[partition],
										  state->repartition_probe_files[partition],
										  state->repartition_hash_shift))
					return true;
			}
			pfree(state->repartition_build_files);
			pfree(state->repartition_probe_files);
			pfree(state->repartition_build_rows);
			pg_batch_spill_destroy(state->repartition_build_spill);
			pg_batch_spill_destroy(state->repartition_probe_spill);
			state->repartition_build_spill = NULL;
			state->repartition_probe_spill = NULL;
			state->repartition_build_files = NULL;
			state->repartition_probe_files = NULL;
			state->repartition_build_rows = NULL;
			state->nrepartitions = 0;
			state->current_repartition = -1;
		}

		while (++state->current_partition < state->npartitions)
		{
			if (state->build_files[state->current_partition] == NULL ||
				state->probe_files[state->current_partition] == NULL)
				continue;
			if (state->build_partition_rows[state->current_partition] >
				state->build_chunk_rows && repartition_current_partition(state))
				break;
			if (start_partition_files(state,
									  state->build_files[state->current_partition],
									  state->probe_files[state->current_partition],
									  state->partition_shift))
				return true;
		}
		if (state->repartition_build_files != NULL)
			continue;
		break;
	}
	state->partition_active = false;
	return false;
}

void
hash_join_finish_spilled_build(BatchHashJoinState *state)
{
	/* Destructive readers release completed build partitions early. */
	pg_batch_spill_finish(state->build_spill, false);
	record_spill_writes(state, state->build_spill);
}

void
hash_join_finish_spilled_probe(BatchHashJoinState *state)
{
	/* Probe partitions are rewound for every in-memory build chunk. */
	pg_batch_spill_finish(state->probe_spill, true);
	record_spill_writes(state, state->probe_spill);
	state->current_partition = -1;
	state->partition_active = false;
}

bool
hash_join_publish_resident_probe(BatchHashJoinState *state, bool final_batch)
{
	SpillBlock *block = &state->resident_probe_block;
	int			nrows;

	if (block->nrows < PG_BATCH_SIZE && !final_batch)
		return false;
	nrows = Min(block->nrows, PG_BATCH_SIZE);
	if (nrows == 0)
		return false;
	for (int column = 0; column < block->ncolumns; column++)
	{
		struct ArrowArray *array = &block->arrays[column];

		array->length = nrows;
		array->offset = 0;
		array->null_count = -1;
	}
	block->selection = pg_batch_nrows_mask(nrows);
	block->batch.nrows = nrows;
	state->probe_batch = &block->batch;
	state->probe_from_spill = true;
	state->probe_from_resident = true;
	state->next_probe_row = -1;
	hash_join_prepare_probe_keys(state);
	return true;
}

void
hash_join_finish_resident_probe(BatchHashJoinState *state)
{
	SpillBlock *block = &state->resident_probe_block;
	int			consumed = block->batch.nrows;
	int			remaining = block->nrows - consumed;
	int			stride = (block->capacity + 7) / 8;

	if (remaining > 0)
	{
		memmove(block->hashes, block->hashes + consumed,
				sizeof(uint32) * remaining);
		for (int column = 0; column < block->ncolumns; column++)
		{
			int32	   *values = &block->values[column * block->capacity];
			uint8	   *validity = &block->validity[column * stride];
			uint8		tmp[(PG_BATCH_SIZE + 7) / 8] = {0};

			memmove(values, values + consumed, sizeof(int32) * remaining);
			for (int row = 0; row < remaining; row++)
			{
				int			source = consumed + row;

				if ((validity[source / 8] &
					 ((uint8) 1 << (source % 8))) != 0)
					tmp[row / 8] |= (uint8) 1 << (row % 8);
			}
			MemSet(validity, 0, stride);
			memcpy(validity, tmp, sizeof(tmp));
		}
	}
	else
	{
		MemSet(block->validity, 0,
			   sizeof(uint8) * block->ncolumns * stride);
	}
	block->nrows = remaining;
	state->probe_from_resident = false;
}

bool
hash_join_fetch_spilled_probe_batch(BatchHashJoinState *state)
{
	for (;;)
	{
		if (!state->partition_active && !start_next_partition(state))
			return false;
		if (read_spill_block(state, state->active_probe_file,
							 &state->probe_block))
		{
			state->probe_batch = &state->probe_block.batch;
			state->probe_from_spill = true;
			state->spill_probe_reads += state->probe_block.nrows;
			MemSet(state->probe_columns, 0,
				   sizeof(InputColumn) * state->nouter_columns);
			state->next_probe_row = -1;
			hash_join_prepare_probe_keys(state);
			return true;
		}
		if (state->build_partition_more && load_build_chunk(state))
		{
			rewind_spill_file(state->active_probe_file);
			continue;
		}
		state->partition_active = false;
	}
}

void
hash_join_close_spill_files(BatchHashJoinState *state)
{
	finish_active_partition(state);
	pg_batch_spill_destroy(state->build_spill);
	pg_batch_spill_destroy(state->probe_spill);
	pg_batch_spill_destroy(state->repartition_build_spill);
	pg_batch_spill_destroy(state->repartition_probe_spill);
	state->build_spill = NULL;
	state->probe_spill = NULL;
	state->repartition_build_spill = NULL;
	state->repartition_probe_spill = NULL;
	state->build_files = NULL;
	state->probe_files = NULL;
	state->repartition_build_files = NULL;
	state->repartition_probe_files = NULL;
}
