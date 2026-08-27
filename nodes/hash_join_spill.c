#include "postgres.h"

#include "executor/executor.h"
#include "storage/buffile.h"
#include "utils/memutils.h"

#include "hash_join_internal.h"

/*
 * Private disk path for PgBatchHashJoin. Files contain 64-row column blocks
 * with a hash per row, one validity word per column, and dense int32 values.
 * Initial partitioning groups a source batch in one pass and buffers small
 * writes. Large partitions can use more hash bits for one extra partitioning
 * level; data that remains skewed is processed by rescanning the probe file.
 */

typedef struct SpillHeader
{
	uint32		magic;
	uint16		nrows;
	uint16		ncolumns;
} SpillHeader;

#define PG_BATCH_SPILL_MAGIC 0x50474248U

/*
 * Repartitioning writes both sides once more. For a few cached probe passes,
 * that extra I/O costs more than it saves.
 */
#define PG_BATCH_REPARTITION_MIN_PASSES 6

static void rewind_spill_file(BufFile *file);

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
spill_prepare_columns(PgBatchBridgeBatch *batch, const Bitmapset *columns,
					  const uint64 *rows,
					  PgBatchBridgeMaterializePhase phase)
{
	/* Spill blocks are already stored as dense native int32 columns. */
}

static void
spill_get_arrow_column(PgBatchBridgeBatch *batch, int column,
					   PgBatchArrowView *view)
{
	SpillBlock *block = batch->private_data;

	if (column < 0 || column >= block->ncolumns)
		elog(ERROR, "pg_batch spill column is out of range");
	view->array = &block->arrays[column];
	view->schema = &block->schemas[column];
}

static const PgBatchBridgeArrowInterface spill_arrow_interface = {
	.abi_version = PG_BATCH_BRIDGE_ARROW_INTERFACE_VERSION,
	.struct_size = sizeof(PgBatchBridgeArrowInterface),
	.get_column = spill_get_arrow_column,
};

static const void *
spill_get_native_interface(PgBatchBridgeBatch *batch, const char *name,
						   uint32 version)
{
	if (version == PG_BATCH_BRIDGE_ARROW_INTERFACE_VERSION &&
		strcmp(name, PG_BATCH_BRIDGE_ARROW_INTERFACE_NAME) == 0)
		return &spill_arrow_interface;
	return NULL;
}

static const PgBatchBridgeBatchOps spill_batch_ops = {
	.abi_version = PG_BATCH_BRIDGE_ABI_VERSION,
	.struct_size = sizeof(PgBatchBridgeBatchOps),
	.prepare_columns = spill_prepare_columns,
	.get_datum_column = NULL,
	.get_native_interface = spill_get_native_interface,
	.release = NULL,
};

static void
init_spill_block(SpillBlock *block, int ncolumns, int capacity,
				 MemoryContext context)
{
	int			validity_bytes = (capacity + 7) / 8;

	block->ncolumns = ncolumns;
	block->capacity = capacity;
	block->hashes = MemoryContextAlloc(context,
									 sizeof(uint32) * capacity);
	block->values = MemoryContextAlloc(context,
									   sizeof(int32) * ncolumns * capacity);
	block->validity = MemoryContextAlloc(context,
										 sizeof(uint8) * ncolumns *
										 validity_bytes);
	block->buffers = MemoryContextAlloc(context,
										sizeof(void *) * ncolumns * 2);
	block->arrays = MemoryContextAllocZero(context,
										   sizeof(struct ArrowArray) * ncolumns);
	block->schemas = MemoryContextAllocZero(context,
											sizeof(struct ArrowSchema) * ncolumns);
	for (int column = 0; column < ncolumns; column++)
	{
		block->buffers[column * 2] =
			&block->validity[column * validity_bytes];
		block->buffers[column * 2 + 1] =
			&block->values[column * capacity];
		block->arrays[column].n_buffers = 2;
		block->arrays[column].buffers = &block->buffers[column * 2];
		block->schemas[column].format = "i";
		block->schemas[column].flags = ARROW_FLAG_NULLABLE;
	}
	block->batch.abi_version = PG_BATCH_BRIDGE_ABI_VERSION;
	block->batch.struct_size = sizeof(PgBatchBridgeBatch);
	block->batch.nwords = 1;
	block->batch.selection = &block->selection;
	block->batch.table_oid = InvalidOid;
	block->batch.ops = &spill_batch_ops;
	block->batch.private_data = block;
}

static bool
read_spill_block(BatchHashJoinState *state, BufFile *file, SpillBlock *block)
{
	int			total_rows = 0;
	int			validity_bytes = (block->capacity + 7) / 8;

	MemSet(block->validity, 0,
		   sizeof(uint8) * block->ncolumns * validity_bytes);
	while (total_rows < PG_BATCH_SIZE)
	{
		SpillHeader header;
		int			fileno;
		pgoff_t		offset;
		size_t		read;

		BufFileTell(file, &fileno, &offset);
		read = BufFileReadMaybeEOF(file, &header, sizeof(header), true);
		if (read == 0)
			break;
		if (read != sizeof(header) || header.magic != PG_BATCH_SPILL_MAGIC ||
			header.nrows == 0 || header.nrows > PG_BATCH_SIZE ||
			header.ncolumns != block->ncolumns)
			elog(ERROR, "invalid pg_batch hash join spill block");
		if (total_rows > 0 && total_rows + header.nrows > block->capacity)
		{
			if (BufFileSeek(file, fileno, offset, SEEK_SET) != 0)
				elog(ERROR, "could not seek pg_batch hash join spill file");
			break;
		}
		BufFileReadExact(file, &block->hashes[total_rows],
						 sizeof(uint32) * header.nrows);
		state->spill_pages_read++;
		for (int column = 0; column < block->ncolumns; column++)
		{
			uint64		validity;

			BufFileReadExact(file, &validity, sizeof(validity));
			BufFileReadExact(file,
							 &block->values[column * block->capacity + total_rows],
							 sizeof(int32) * header.nrows);
			for (int row = 0; row < header.nrows; row++)
			{
				int			output_row = total_rows + row;

				if ((validity & (UINT64CONST(1) << row)) != 0)
					block->validity[column * validity_bytes +
									output_row / 8] |=
						(uint8) 1 << (output_row % 8);
			}
		}
		total_rows += header.nrows;
	}
	if (total_rows == 0)
		return false;
	block->nrows = total_rows;
	for (int column = 0; column < block->ncolumns; column++)
	{
		struct ArrowArray *array = &block->arrays[column];

		array->length = block->nrows;
		array->offset = 0;
		array->null_count = 0;
		for (int row = 0; row < block->nrows; row++)
		{
			if ((block->validity[column * validity_bytes + row / 8] &
				 ((uint8) 1 << (row % 8))) == 0)
				array->null_count++;
		}
	}
	block->selection = pg_batch_nrows_mask(block->nrows);
	block->batch.nrows = block->nrows;
	block->batch.consumed = false;
	return true;
}

static uint64
spill_block_bytes(int ncolumns, int nrows)
{
	return sizeof(SpillHeader) + sizeof(uint32) * nrows +
		ncolumns * (sizeof(uint64) + sizeof(int32) * nrows);
}

static BufFile *
spill_file(BufFile **file)
{
	if (*file == NULL)
		*file = BufFileCreateTemp(false);
	return *file;
}

static void
write_input_block(BatchHashJoinState *state, BufFile *file,
				  InputColumn *columns, int ncolumns, const int *rows,
				  const uint32 *hashes, int nrows)
{
	SpillHeader header = {PG_BATCH_SPILL_MAGIC, nrows, ncolumns};
	uint32		block_hashes[PG_BATCH_SIZE];
	int32		values[PG_BATCH_SIZE];

	BufFileWrite(file, &header, sizeof(header));
	for (int i = 0; i < nrows; i++)
		block_hashes[i] = hashes[rows[i]];
	BufFileWrite(file, block_hashes, sizeof(uint32) * nrows);
	for (int column = 0; column < ncolumns; column++)
	{
		uint64		validity = 0;

		for (int i = 0; i < nrows; i++)
		{
			bool		isnull;

			values[i] = hash_join_input_value(&columns[column], rows[i], &isnull);
			if (!isnull)
				validity |= UINT64CONST(1) << i;
		}
		BufFileWrite(file, &validity, sizeof(validity));
		BufFileWrite(file, values, sizeof(int32) * nrows);
	}
	state->spill_bytes += spill_block_bytes(ncolumns, nrows);
	state->spill_pages_written++;
}

static void
flush_spill_buffer(BatchHashJoinState *state, BufFile **files,
				   int partition, int ncolumns)
{
	int			nrows = state->spill_buffer_counts[partition];
	SpillHeader header = {PG_BATCH_SPILL_MAGIC, nrows, ncolumns};
	uint32	   *hashes = &state->spill_buffer_hashes[
												 partition * PG_BATCH_SIZE];

	if (nrows == 0)
		return;
	BufFileWrite(spill_file(&files[partition]), &header, sizeof(header));
	BufFileWrite(files[partition], hashes, sizeof(uint32) * nrows);
	for (int column = 0; column < ncolumns; column++)
	{
		uint64	   *validity = &state->spill_buffer_validity[
			partition * state->spill_buffer_columns + column];
		int32	   *values = &state->spill_buffer_values[
			(partition * state->spill_buffer_columns + column) * PG_BATCH_SIZE];

		BufFileWrite(files[partition], validity, sizeof(uint64));
		BufFileWrite(files[partition], values, sizeof(int32) * nrows);
		*validity = 0;
	}
	state->spill_bytes += spill_block_bytes(ncolumns, nrows);
	state->spill_pages_written++;
	state->spill_buffer_counts[partition] = 0;
}

static void
write_partition_rows(BatchHashJoinState *state, BufFile **files,
					 int partition, InputColumn *columns, int ncolumns,
					 const int *rows, const uint32 *hashes, int nrows)
{
	if (!state->spill_buffered)
	{
		write_input_block(state, spill_file(&files[partition]), columns,
						  ncolumns, rows, hashes, nrows);
		return;
	}
	for (int i = 0; i < nrows; i++)
	{
		int			output = state->spill_buffer_counts[partition]++;

		state->spill_buffer_hashes[
			partition * PG_BATCH_SIZE + output] =
			hashes[rows[i]];
		for (int column = 0; column < ncolumns; column++)
		{
			bool		isnull;
			int32		value = hash_join_input_value(&columns[column], rows[i], &isnull);
			int			buffer_column =
				partition * state->spill_buffer_columns + column;

			state->spill_buffer_values[
				buffer_column * PG_BATCH_SIZE + output] =
				value;
			if (!isnull)
				state->spill_buffer_validity[
					buffer_column] |= UINT64CONST(1) << output;
		}
		if (state->spill_buffer_counts[partition] == PG_BATCH_SIZE)
			flush_spill_buffer(state, files, partition, ncolumns);
	}
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

static void
flush_spill_buffers(BatchHashJoinState *state, BufFile **files, int ncolumns)
{
	if (!state->spill_buffered)
		return;
	for (int partition = 0; partition < state->npartitions; partition++)
		flush_spill_buffer(state, files, partition, ncolumns);
}

uint64
hash_join_spill_input_batch(BatchHashJoinState *state, PgBatchBridgeBatch *batch,
							InputColumn *columns, int ncolumns, const int *keys,
							const int *batch_columns, BufFile **files,
							uint64 *resident_rows)
{
	uint32	   *hashes;
	bool	   *hash_valid;
	int		   *next_rows;
	int		   *used_partitions;
	int			nused = 0;
	uint64		accepted = 0;

	hashes = MemoryContextAlloc(state->join_context,
								sizeof(uint32) * batch->nrows);
	hash_valid = MemoryContextAlloc(state->join_context,
									sizeof(bool) * batch->nrows);
	next_rows = MemoryContextAlloc(state->join_context,
								   sizeof(int) * batch->nrows);
	used_partitions = MemoryContextAlloc(state->join_context,
										 sizeof(int) * batch->nrows);
	for (int row = 0; row < batch->nrows; row++)
		hash_valid[row] = false;
	if (resident_rows != NULL)
		*resident_rows = 0;
	/* Hash each selected row once, independently of the partition count. */
	for (int row = -1;
		 (row = pg_batch_bridge_next_selected(batch, row)) >= 0;)
	{
		hash_valid[row] = hash_join_input_row_hash(columns, keys, state->nkeys,
												   row, &hashes[row]);
		if (hash_valid[row])
		{
			accepted++;
			if (files == state->build_files && state->bloom_words != NULL)
				bloom_add(state, hashes[row]);
			else if (files == state->probe_files && state->bloom_usable &&
					 !bloom_may_contain(state, hashes[row]))
			{
				hash_valid[row] = false;
				state->bloom_rejected_rows++;
			}
			else if (files == state->probe_files)
			{
				int			partition = hashes[row] &
					(state->npartitions - 1);

				if (!bms_is_member(partition, state->resident_partitions) &&
					state->build_files[partition] == NULL)
					hash_valid[row] = false;
			}
		}
	}
	if (files == state->probe_files)
	{
		uint64		survivors = 0;

		/* Do not materialize probe payload rejected by the complete key filter. */
		for (int row = -1;
			 (row = pg_batch_bridge_next_selected(batch, row)) >= 0;)
		{
			if (hash_valid[row])
				survivors |= UINT64CONST(1) << row;
		}
		for (int column = 0; column < ncolumns && survivors != 0; column++)
		{
			if (!columns[column].prepared)
				hash_join_load_input_column(batch, batch_columns[column],
					&survivors, PG_BATCH_PROJECT_PHASE, &columns[column],
					state->join_context);
		}
	}
	/* Link rows by partition without rescanning the batch per partition. */
	for (int row = -1;
		 (row = pg_batch_bridge_next_selected(batch, row)) >= 0;)
	{
		int			partition;

		if (!hash_valid[row])
			continue;
		partition = hashes[row] & (state->npartitions - 1);
		if (bms_is_member(partition, state->resident_partitions))
		{
			if (files == state->probe_files)
				buffer_resident_probe_row(state, columns, row, hashes[row]);
			else if (resident_rows != NULL)
				*resident_rows |= UINT64CONST(1) << row;
			continue;
		}
		if (files == state->build_files)
		{
			state->build_partition_rows[partition]++;
			state->spill_build_rows++;
		}
		if (files == state->probe_files &&
			state->build_files[partition] == NULL)
			continue;
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
	pfree(hash_valid);
	pfree(hashes);
	return accepted;
}

static void
write_store_block(BatchHashJoinState *state, BufFile *file,
				  const int *rows, int nrows)
{
	BuildStore *store = &state->build;
	SpillHeader header = {PG_BATCH_SPILL_MAGIC, nrows, store->ncolumns};
	uint32		hashes[PG_BATCH_SIZE];
	int32		values[PG_BATCH_SIZE];

	BufFileWrite(file, &header, sizeof(header));
	for (int i = 0; i < nrows; i++)
		hashes[i] = store->links[rows[i]].hash;
	BufFileWrite(file, hashes, sizeof(uint32) * nrows);
	for (int column = 0; column < store->ncolumns; column++)
	{
		uint64		validity = 0;

		for (int i = 0; i < nrows; i++)
		{
			uint32		row = rows[i];

			values[i] = store->columns[column].values[row];
			if ((store->columns[column].validity[row / 64] &
				 (UINT64CONST(1) << (row % 64))) != 0)
				validity |= UINT64CONST(1) << i;
		}
		BufFileWrite(file, &validity, sizeof(validity));
		BufFileWrite(file, values, sizeof(int32) * nrows);
	}
	state->spill_bytes += spill_block_bytes(store->ncolumns, nrows);
	state->spill_pages_written++;
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
								  spill_file(&state->build_files[partition]),
								  rows, count);
				count = 0;
			}
		}
		if (count > 0)
			write_store_block(state,
							  spill_file(&state->build_files[partition]),
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
write_spill_rows(BatchHashJoinState *state, BufFile **files, int partition,
				 SpillBlock *block, int ncolumns, const int *rows, int nrows)
{
	SpillHeader header = {PG_BATCH_SPILL_MAGIC, nrows, ncolumns};
	uint32		hashes[PG_BATCH_SIZE];
	int32		values[PG_BATCH_SIZE];

	BufFileWrite(spill_file(&files[partition]), &header, sizeof(header));
	for (int i = 0; i < nrows; i++)
		hashes[i] = block->hashes[rows[i]];
	BufFileWrite(files[partition], hashes, sizeof(uint32) * nrows);
	for (int column = 0; column < ncolumns; column++)
	{
		uint64		validity = 0;

		for (int i = 0; i < nrows; i++)
		{
			int			row = rows[i];

			values[i] = hash_join_spill_value(block, column, row);
			if (hash_join_spill_value_valid(block, column, row))
				validity |= UINT64CONST(1) << i;
		}
		BufFileWrite(files[partition], &validity, sizeof(validity));
		BufFileWrite(files[partition], values, sizeof(int32) * nrows);
	}
	state->spill_bytes += spill_block_bytes(ncolumns, nrows);
	state->spill_pages_written++;
}

static void
repartition_file(BatchHashJoinState *state, BufFile *source,
				 SpillBlock *block, int ncolumns, BufFile **files,
				 BufFile **matching_build_files, uint64 *build_rows,
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

	state->spill_buffered = false;
	state->spill_buffer_counts = NULL;
	state->spill_buffer_hashes = NULL;
	state->spill_buffer_values = NULL;
	state->spill_buffer_validity = NULL;
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
												sizeof(BufFile *) * state->npartitions);
	state->probe_files = MemoryContextAllocZero(state->spill_context,
												sizeof(BufFile *) * state->npartitions);
	state->build_partition_rows = MemoryContextAllocZero(state->spill_context,
													 sizeof(uint64) * state->npartitions);
	state->resident_partition_rows = MemoryContextAllocZero(
		state->spill_context, sizeof(uint64) * state->npartitions);
	state->spill_partition_heads = MemoryContextAlloc(state->spill_context,
													  sizeof(int) * state->npartitions);
	state->spill_partition_tails = MemoryContextAlloc(state->spill_context,
													  sizeof(int) * state->npartitions);
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
		if (bytes <= state->memory_limit / 4)
		{
			state->spill_buffered = true;
			state->spill_buffer_columns = columns;
			state->spill_buffer_counts = MemoryContextAllocZero(
																state->spill_context, sizeof(int) * state->npartitions);
			state->spill_buffer_hashes = MemoryContextAlloc(
														state->spill_context,
														sizeof(uint32) * state->npartitions * PG_BATCH_SIZE);
			state->spill_buffer_values = MemoryContextAlloc(
														state->spill_context,
														sizeof(int32) * state->npartitions * columns * PG_BATCH_SIZE);
			state->spill_buffer_validity = MemoryContextAllocZero(
															  state->spill_context,
															  sizeof(uint64) * state->npartitions * columns);
		}
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
					 state->spill_context);
	init_spill_block(&state->probe_block, state->nouter_columns,
					 PG_BATCH_SIZE,
					 state->spill_context);
	init_spill_block(&state->resident_probe_block, state->nouter_columns,
					 PG_BATCH_SIZE * 2, state->spill_context);
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
	BufFile    *file = state->active_build_file;

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
rewind_spill_file(BufFile *file)
{
	if (BufFileSeek(file, 0, 0, SEEK_SET) != 0)
		elog(ERROR, "could not rewind pg_batch hash join spill file");
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
start_partition_files(BatchHashJoinState *state, BufFile *build_file,
					  BufFile *probe_file, int hash_shift)
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
	BufFile    *build_file = state->build_files[state->current_partition];
	BufFile    *probe_file = state->probe_files[state->current_partition];

	/*
	 * Below this point another full write costs more than cached probe
	 * passes.
	 */
	if (required < PG_BATCH_REPARTITION_MIN_PASSES)
		return false;
	npartitions = next_partition_count(required);
	state->repartition_build_files = MemoryContextAllocZero(
															state->spill_context, sizeof(BufFile *) * npartitions);
	state->repartition_probe_files = MemoryContextAllocZero(
															state->spill_context, sizeof(BufFile *) * npartitions);
	state->repartition_build_rows = MemoryContextAllocZero(
														   state->spill_context, sizeof(uint64) * npartitions);
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
	BufFileClose(build_file);
	BufFileClose(probe_file);
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

		if (state->repartition_build_files[partition] != NULL)
			BufFileClose(state->repartition_build_files[partition]);
		if (state->repartition_probe_files[partition] != NULL)
			BufFileClose(state->repartition_probe_files[partition]);
		state->repartition_build_files[partition] = NULL;
		state->repartition_probe_files[partition] = NULL;
	}
}

static bool
start_next_partition(BatchHashJoinState *state)
{
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
	/* Build buffers must be visible before probing starts. */
	flush_spill_buffers(state, state->build_files, state->ninner_columns);
}

void
hash_join_finish_spilled_probe(BatchHashJoinState *state)
{
	flush_spill_buffers(state, state->probe_files, state->nouter_columns);
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
	block->batch.consumed = false;
	state->probe_slot = NULL;
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
			state->probe_slot = NULL;
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
	for (int partition = 0; partition < state->npartitions; partition++)
	{
		if (state->build_files != NULL && state->build_files[partition] != NULL)
		{
			BufFileClose(state->build_files[partition]);
			state->build_files[partition] = NULL;
		}
		if (state->probe_files != NULL && state->probe_files[partition] != NULL)
		{
			BufFileClose(state->probe_files[partition]);
			state->probe_files[partition] = NULL;
		}
	}
	for (int partition = 0; partition < state->nrepartitions; partition++)
	{
		if (state->repartition_build_files != NULL &&
			state->repartition_build_files[partition] != NULL)
		{
			BufFileClose(state->repartition_build_files[partition]);
			state->repartition_build_files[partition] = NULL;
		}
		if (state->repartition_probe_files != NULL &&
			state->repartition_probe_files[partition] != NULL)
		{
			BufFileClose(state->repartition_probe_files[partition]);
			state->repartition_probe_files[partition] = NULL;
		}
	}
}
