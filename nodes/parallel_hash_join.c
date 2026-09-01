#include "postgres.h"

#include "access/parallel.h"
#include "catalog/pg_type_d.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/barrier.h"
#include "storage/dsm.h"
#include "storage/shm_toc.h"
#include "storage/spin.h"
#include "utils/dsa.h"
#include "utils/memutils.h"

#include "hash_join_internal.h"

/*
 * Parallel hash join keeps the build payload in column-major DSA chunks.
 * Chunks are private while being filled and immutable after publication.
 * A shared bucket array links encoded (chunk, row) identifiers, so probes do
 * not copy build rows into backend-local memory.
 *
 * If the planner requested several partitions, or the combined DSA budget is
 * exceeded while collecting chunks, all participants use the common shared
 * spill service. Each participant owns its writer files; partitions are
 * claimed once and processed by one backend after both sides are complete.
 */

#define PARALLEL_BUILD_CHUNK_ROWS 1024
#define PARALLEL_REPARTITION_MIN_PASSES 6

typedef struct ParallelBuildChunk
{
	dsa_pointer next;
	uint32		nrows;
	uint32		index;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} ParallelBuildChunk;

typedef struct ParallelHashShared
{
	dsm_handle	segment_handle;
	int			max_participants;
	int			ninner_columns;
	int			nouter_columns;
	int			npartitions;
	Size		build_spill_offset;
	Size		probe_spill_offset;
	Size		memory_limit;

	Barrier		build_barrier;
	slock_t		chunk_lock;
	dsa_pointer chunks;
	dsa_pointer directory;
	dsa_pointer buckets;
	uint32		nchunks;
	uint32		nbuckets;

	pg_atomic_uint64 allocated_bytes;
	pg_atomic_uint64 build_rows;
	pg_atomic_uint32 spilled;
	pg_atomic_uint32 has_duplicates;
	pg_atomic_uint32 next_chunk;
	pg_atomic_uint32 next_partition;
} ParallelHashShared;

struct ParallelHashJoinLocal
{
	ParallelHashShared *shared;
	dsa_area   *area;
	int			participant;
	bool		attached;
	bool		local_build;

	dsa_pointer chunk_pointer;
	ParallelBuildChunk *chunk;
	dsa_pointer *directory;
	pg_atomic_uint32 *buckets;

	PgBatchSharedSpill *build_spill;
	PgBatchSharedSpill *probe_spill;
	PgBatchSharedSpillWriter *build_writer;
	PgBatchSharedSpillWriter *probe_writer;
	PgBatchSharedSpillReader *build_reader;
	PgBatchSharedSpillReader *probe_reader;
	PgBatchSpillSet *repartition_build;
	PgBatchSpillSet *repartition_probe;
	PgBatchSpillReader *repartition_build_reader;
	PgBatchSpillReader *repartition_probe_reader;
	int			current_partition;
	int			current_repartition;
	int			nrepartitions;
	int			repartition_shift;
	uint32		build_chunk_rows;
	bool		build_maybe_more;
	uint64		explain_build_rows;
	uint32		explain_buckets;
	uint32		explain_chunks;
	uint64		explain_shared_memory;
	int			explain_partitions;
	bool		explain_spilled;
	bool		build_has_duplicates;
};

static ParallelBuildChunk *row_chunk(BatchHashJoinState *state, uint32 row,
	uint32 *offset);

static Size
build_chunk_size(int ncolumns)
{
	Size		size = MAXALIGN(sizeof(ParallelBuildChunk));

	size = add_size(size,
		MAXALIGN(sizeof(uint32) * PARALLEL_BUILD_CHUNK_ROWS));
	size = add_size(size,
		MAXALIGN(sizeof(uint32) * PARALLEL_BUILD_CHUNK_ROWS));
	size = add_size(size,
		MAXALIGN(sizeof(uint64) * ncolumns *
				 (PARALLEL_BUILD_CHUNK_ROWS / 64)));
	size = add_size(size,
		MAXALIGN(sizeof(int32) * ncolumns * PARALLEL_BUILD_CHUNK_ROWS));
	return size;
}

static uint32 *
chunk_hashes(ParallelBuildChunk *chunk)
{
	return (uint32 *) ((char *) chunk + MAXALIGN(sizeof(*chunk)));
}

static uint32 *
chunk_links(ParallelBuildChunk *chunk)
{
	return chunk_hashes(chunk) + PARALLEL_BUILD_CHUNK_ROWS;
}

static uint64 *
chunk_validity(ParallelBuildChunk *chunk)
{
	return (uint64 *) (chunk_links(chunk) + PARALLEL_BUILD_CHUNK_ROWS);
}

static int32 *
chunk_values(ParallelBuildChunk *chunk, int ncolumns)
{
	return (int32 *) (chunk_validity(chunk) +
		ncolumns * (PARALLEL_BUILD_CHUNK_ROWS / 64));
}

static Size
shared_header_size(void)
{
	return MAXALIGN(sizeof(ParallelHashShared));
}

static int
next_partition_count(uint64 required)
{
	int			result = 2;

	/* Match the serial join's bound on partition metadata and writers. */
	while ((uint64) result < required && result < 1024)
		result <<= 1;
	return result;
}

static int
shared_partition_count(BatchHashJoinState *state, int participants)
{
	return next_partition_count(Max(state->planned_partitions,
									participants * 2));
}

static ParallelHashJoinLocal *
parallel_local(BatchHashJoinState *state)
{
	if (state->parallel == NULL)
		state->parallel = MemoryContextAllocZero(state->join_context,
											 sizeof(*state->parallel));
	return state->parallel;
}

static PgBatchSharedSpill *
shared_build_spill(ParallelHashShared *shared)
{
	return (PgBatchSharedSpill *) ((char *) shared +
		shared->build_spill_offset);
}

static PgBatchSharedSpill *
shared_probe_spill(ParallelHashShared *shared)
{
	return (PgBatchSharedSpill *) ((char *) shared +
		shared->probe_spill_offset);
}

Size
parallel_hash_join_estimate_dsm(CustomScanState *node, ParallelContext *pcxt)
{
	BatchHashJoinState *state = (BatchHashJoinState *) node;
	int			participants = pcxt->nworkers + 1;
	int			partitions = shared_partition_count(state, participants);
	Size		size = shared_header_size();

	size = add_size(size, MAXALIGN(pg_batch_shared_spill_estimate(
		participants, partitions)));
	size = add_size(size, MAXALIGN(pg_batch_shared_spill_estimate(
		participants, partitions)));
	return size;
}

static void
initialize_shared_state(BatchHashJoinState *state, ParallelContext *pcxt,
						void *coordinate)
{
	ParallelHashShared *shared = coordinate;
	int			participants = pcxt->nworkers + 1;
	Size		one_spill;
	Size		combined_limit;

	MemSet(shared, 0, parallel_hash_join_estimate_dsm(&state->css, pcxt));
	shared->segment_handle = dsm_segment_handle(pcxt->seg);
	shared->max_participants = participants;
	shared->ninner_columns = state->ninner_columns;
	shared->nouter_columns = state->nouter_columns;
	shared->npartitions = shared_partition_count(state, participants);
	combined_limit = state->memory_limit > SIZE_MAX / participants ?
		SIZE_MAX : state->memory_limit * participants;
	shared->memory_limit = combined_limit;
	shared->build_spill_offset = shared_header_size();
	one_spill = MAXALIGN(pg_batch_shared_spill_estimate(participants,
		shared->npartitions));
	shared->probe_spill_offset = shared->build_spill_offset + one_spill;
	BarrierInit(&shared->build_barrier, 0);
	SpinLockInit(&shared->chunk_lock);
	pg_atomic_init_u64(&shared->allocated_bytes, 0);
	pg_atomic_init_u64(&shared->build_rows, 0);
	pg_atomic_init_u32(&shared->spilled,
		state->planned_partitions > 1 ? 1 : 0);
	pg_atomic_init_u32(&shared->has_duplicates, 0);
	pg_atomic_init_u32(&shared->next_chunk, 0);
	pg_atomic_init_u32(&shared->next_partition, 0);
	pg_batch_shared_spill_initialize(shared_build_spill(shared), pcxt->seg,
		participants, shared->npartitions, state->ninner_columns);
	pg_batch_shared_spill_initialize(shared_probe_spill(shared), pcxt->seg,
		participants, shared->npartitions, state->nouter_columns);
}

void
parallel_hash_join_initialize_dsm(CustomScanState *node,
								  ParallelContext *pcxt, void *coordinate)
{
	BatchHashJoinState *state = (BatchHashJoinState *) node;
	ParallelHashJoinLocal *local;

	/* A private-memory fallback has no query DSA and uses the serial path. */
	if (pcxt->seg == NULL)
		return;
	initialize_shared_state(state, pcxt, coordinate);
	local = parallel_local(state);
	local->shared = coordinate;
	local->participant = 0;
	local->build_spill = shared_build_spill(coordinate);
	local->probe_spill = shared_probe_spill(coordinate);
}

void
parallel_hash_join_reinitialize_dsm(CustomScanState *node,
									ParallelContext *pcxt, void *coordinate)
{
	BatchHashJoinState *state = (BatchHashJoinState *) node;
	ParallelHashShared *shared = coordinate;

	if (pcxt->seg == NULL)
		return;
	pg_batch_shared_spill_reset(shared_build_spill(shared));
	pg_batch_shared_spill_reset(shared_probe_spill(shared));
	shared->chunks = InvalidDsaPointer;
	shared->directory = InvalidDsaPointer;
	shared->buckets = InvalidDsaPointer;
	shared->nchunks = 0;
	shared->nbuckets = 0;
	BarrierInit(&shared->build_barrier, 0);
	SpinLockInit(&shared->chunk_lock);
	pg_atomic_init_u64(&shared->allocated_bytes, 0);
	pg_atomic_init_u64(&shared->build_rows, 0);
	pg_atomic_init_u32(&shared->spilled,
		state->planned_partitions > 1 ? 1 : 0);
	pg_atomic_init_u32(&shared->has_duplicates, 0);
	pg_atomic_init_u32(&shared->next_chunk, 0);
	pg_atomic_init_u32(&shared->next_partition, 0);
}

void
parallel_hash_join_initialize_worker(CustomScanState *node, shm_toc *toc,
									 void *coordinate)
{
	BatchHashJoinState *state = (BatchHashJoinState *) node;
	ParallelHashJoinLocal *local = parallel_local(state);
	ParallelHashShared *shared = coordinate;
	dsm_segment *segment = dsm_find_mapping(shared->segment_handle);

	(void) toc;
	if (segment == NULL)
		elog(ERROR, "pg_batch cannot find the parallel hash DSM segment");
	local->shared = shared;
	local->participant = ParallelWorkerNumber + 1;
	local->build_spill = pg_batch_shared_spill_attach(
		shared_build_spill(shared), segment);
	local->probe_spill = pg_batch_shared_spill_attach(
		shared_probe_spill(shared), segment);
}

static TupleDesc
int4_tuple_desc(int ncolumns)
{
	TupleDesc	desc = CreateTemplateTupleDesc(ncolumns);

	for (int column = 0; column < ncolumns; column++)
		TupleDescInitEntry(desc, column + 1, NULL, INT4OID, -1, 0);
	TupleDescFinalize(desc);
	return desc;
}

static PgBatchSharedSpillWriter *
get_writer(BatchHashJoinState *state, bool build)
{
	ParallelHashJoinLocal *local = state->parallel;
	PgBatchSharedSpillWriter **writer = build ?
		&local->build_writer : &local->probe_writer;

	if (*writer == NULL)
	{
		int			ncolumns = build ? state->ninner_columns :
			state->nouter_columns;
		TupleDesc	desc = int4_tuple_desc(ncolumns);

		*writer = pg_batch_shared_spill_writer_create(state->spill_context,
			build ? local->build_spill : local->probe_spill,
			local->participant, desc, state->memory_limit / 4);
		FreeTupleDesc(desc);
	}
	return *writer;
}

static void
publish_chunk(BatchHashJoinState *state)
{
	ParallelHashJoinLocal *local = state->parallel;
	ParallelHashShared *shared = local->shared;
	ParallelBuildChunk *chunk = local->chunk;

	if (chunk == NULL)
		return;
	if (chunk->nrows == 0)
		dsa_free(local->area, local->chunk_pointer);
	else
	{
		SpinLockAcquire(&shared->chunk_lock);
		chunk->next = shared->chunks;
		shared->chunks = local->chunk_pointer;
		shared->nchunks++;
		SpinLockRelease(&shared->chunk_lock);
		pg_atomic_fetch_add_u64(&shared->build_rows, chunk->nrows);
	}
	local->chunk = NULL;
	local->chunk_pointer = InvalidDsaPointer;
}

static bool
start_chunk(BatchHashJoinState *state)
{
	ParallelHashJoinLocal *local = state->parallel;
	ParallelHashShared *shared = local->shared;
	Size		size = build_chunk_size(state->ninner_columns);
	/* A power-of-two table below 50% load can use almost four heads per row. */
	Size		reservation = add_size(size,
		add_size(sizeof(dsa_pointer),
			4 * sizeof(pg_atomic_uint32) * PARALLEL_BUILD_CHUNK_ROWS));
	uint64		old;

	if (pg_atomic_read_u32(&shared->spilled))
		return false;
	old = pg_atomic_fetch_add_u64(&shared->allocated_bytes, reservation);
	if (old > shared->memory_limit ||
		reservation > shared->memory_limit - old)
	{
		pg_atomic_write_u32(&shared->spilled, 1);
		return false;
	}
	local->chunk_pointer = dsa_allocate0(local->area, size);
	local->chunk = dsa_get_address(local->area, local->chunk_pointer);
	return true;
}

static uint64
append_chunk_rows(BatchHashJoinState *state, InputColumn *columns,
				  const uint32 *hashes, uint64 rows)
{
	ParallelHashJoinLocal *local = state->parallel;

	while (rows != 0)
	{
		int			row;
		int			dest;
		ParallelBuildChunk *chunk;
		int32	   *values;
		uint64	   *validity;

		if (local->chunk == NULL && !start_chunk(state))
			break;
		chunk = local->chunk;
		row = pg_rightmost_one_pos64(rows);
		dest = chunk->nrows++;
		chunk_hashes(chunk)[dest] = hashes[row];
		values = chunk_values(chunk, state->ninner_columns);
		validity = chunk_validity(chunk);
		for (int column = 0; column < state->ninner_columns; column++)
		{
			bool		isnull;
			int32		value = hash_join_input_value(&columns[column], row,
				&isnull);

			values[column * PARALLEL_BUILD_CHUNK_ROWS + dest] = value;
			if (!isnull)
				validity[column * (PARALLEL_BUILD_CHUNK_ROWS / 64) +
					 dest / 64] |= UINT64CONST(1) << (dest % 64);
		}
		rows &= rows - 1;
		if (chunk->nrows == PARALLEL_BUILD_CHUNK_ROWS)
			publish_chunk(state);
	}
	return rows;
}

static void
spill_vectors(BatchHashJoinState *state, PgBatch *batch,
			  InputColumn *columns, int ncolumns, const uint32 *hashes,
			  uint64 rows, bool build)
{
	ParallelHashJoinLocal *local = state->parallel;
	ParallelHashShared *shared = local->shared;
	PgBatchSharedSpillWriter *writer = get_writer(state, build);
	PgBatchInt4Vector *vectors = palloc_array(PgBatchInt4Vector, ncolumns);
	uint64	   *partitions = palloc0_array(uint64, shared->npartitions);

	(void) batch;
	for (int column = 0; column < ncolumns; column++)
		vectors[column] = columns[column].vector;
	for (uint64 pending = rows; pending != 0; pending &= pending - 1)
	{
		int			row = pg_rightmost_one_pos64(pending);
		int			partition = hashes[row] & (shared->npartitions - 1);

		partitions[partition] |= UINT64CONST(1) << row;
	}
	for (int partition = 0; partition < shared->npartitions; partition++)
	{
		PgBatchSelection selected;

		if (partitions[partition] == 0)
			continue;
		selected.nrows = batch->selection.nrows;
		selected.nwords = 1;
		selected.words = &partitions[partition];
		pg_batch_shared_spill_write(writer, partition, vectors, &selected,
			hashes);
	}
	pfree(partitions);
	pfree(vectors);
}

static uint64
hash_batch(BatchHashJoinState *state, PgBatch *batch, InputColumn *columns,
		   const int *keys, uint32 *hashes)
{
	PgBatchSelection valid = batch->selection;
	uint64		rows;

	for (int key = 0; key < state->nkeys; key++)
		state->hash_keys[key] = &columns[keys[key]].vector;
	valid.words = &rows;
	pg_batch_hash_int4(state->hash_keys, state->nkeys, &batch->selection,
		hashes, &valid);
	return rows;
}

static void
collect_build(BatchHashJoinState *state)
{
	ParallelHashJoinLocal *local = state->parallel;
	ParallelHashShared *shared = local->shared;

	for (;;)
	{
		PgBatchInputResult input;
		PgBatch   *batch;
		InputColumn *columns;
		uint32		hashes[PG_BATCH_SIZE];
		uint64		rows;

		if (!pg_batch_input_next(state->inner_input, &input))
			break;
		batch = input.batch;
		state->build_batches++;
		state->build_input_rows += pg_batch_row_count(batch);
		columns = MemoryContextAllocZero(state->join_context,
			sizeof(InputColumn) * state->ninner_columns);
		for (int column = 0; column < state->ninner_columns; column++)
			hash_join_load_input_column(batch,
				state->inner_batch_columns[column], &batch->selection,
				PG_BATCH_PROJECT_PHASE, &columns[column], state->join_context);
		rows = hash_batch(state, batch, columns, state->inner_keys, hashes);
		if (pg_atomic_read_u32(&shared->spilled))
		{
			publish_chunk(state);
			spill_vectors(state, batch, columns, state->ninner_columns,
				hashes, rows, true);
			pg_atomic_fetch_add_u64(&shared->build_rows,
				pg_popcount64(rows));
		}
		else
		{
			rows = append_chunk_rows(state, columns, hashes, rows);
			if (rows != 0 && pg_atomic_read_u32(&shared->spilled))
			{
				/* The reservation that failed left this batch unconsumed. */
				publish_chunk(state);
				spill_vectors(state, batch, columns, state->ninner_columns,
					hashes, rows, true);
				pg_atomic_fetch_add_u64(&shared->build_rows,
					pg_popcount64(rows));
			}
		}
		pfree(columns);
		pg_batch_input_finish(state->inner_input);
	}
	publish_chunk(state);
}

static uint32
choose_bucket_count(uint64 rows)
{
	uint32		result = 1024;

	while ((uint64) result < rows * 2 && result < (UINT32_MAX / 2 + 1))
		result <<= 1;
	return result;
}

static void
setup_shared_hash(BatchHashJoinState *state)
{
	ParallelHashJoinLocal *local = state->parallel;
	ParallelHashShared *shared = local->shared;
	dsa_pointer pointer = shared->chunks;
	dsa_pointer *directory;
	uint32		index = 0;

	if (shared->nchunks > 0)
	{
		shared->directory = dsa_allocate(local->area,
			sizeof(dsa_pointer) * shared->nchunks);
		directory = dsa_get_address(local->area, shared->directory);
		while (DsaPointerIsValid(pointer))
		{
			ParallelBuildChunk *chunk = dsa_get_address(local->area, pointer);

			chunk->index = index;
			directory[index++] = pointer;
			pointer = chunk->next;
		}
	}
	Assert(index == shared->nchunks);
	if (!pg_atomic_read_u32(&shared->spilled))
	{
		shared->nbuckets = choose_bucket_count(
			pg_atomic_read_u64(&shared->build_rows));
		shared->buckets = dsa_allocate0(local->area,
			sizeof(pg_atomic_uint32) * shared->nbuckets);
		directory = dsa_get_address(local->area, shared->buckets);
		for (uint32 bucket = 0; bucket < shared->nbuckets; bucket++)
			pg_atomic_init_u32(&((pg_atomic_uint32 *) directory)[bucket], 0);
	}
}

static void
spill_chunk(BatchHashJoinState *state, ParallelBuildChunk *chunk)
{
	ParallelHashJoinLocal *local = state->parallel;
	ParallelHashShared *shared = local->shared;
	PgBatchSharedSpillWriter *writer = get_writer(state, true);
	uint32	   *hashes = chunk_hashes(chunk);
	int32	   *values = chunk_values(chunk, state->ninner_columns);
	uint64	   *validity = chunk_validity(chunk);
	uint32		out_hashes[PG_BATCH_SPILL_BLOCK_ROWS];
	int32	   *out_values = palloc(sizeof(int32) * state->ninner_columns *
		PG_BATCH_SPILL_BLOCK_ROWS);
	uint64	   *out_validity = palloc0(sizeof(uint64) *
		state->ninner_columns);

	for (int partition = 0; partition < shared->npartitions; partition++)
	{
		int			count = 0;

		MemSet(out_validity, 0, sizeof(uint64) * state->ninner_columns);
		for (uint32 row = 0; row < chunk->nrows; row++)
		{
			if ((hashes[row] & (shared->npartitions - 1)) != partition)
				continue;
			out_hashes[count] = hashes[row];
			for (int column = 0; column < state->ninner_columns; column++)
			{
				out_values[column * PG_BATCH_SPILL_BLOCK_ROWS + count] =
					values[column * PARALLEL_BUILD_CHUNK_ROWS + row];
				if ((validity[column * (PARALLEL_BUILD_CHUNK_ROWS / 64) +
					 row / 64] & (UINT64CONST(1) << (row % 64))) != 0)
					out_validity[column] |= UINT64CONST(1) << count;
			}
			count++;
			if (count == PG_BATCH_SPILL_BLOCK_ROWS)
			{
				pg_batch_shared_spill_write_dense(writer, partition, count,
					out_hashes, out_values, PG_BATCH_SPILL_BLOCK_ROWS,
					out_validity);
				count = 0;
				MemSet(out_validity, 0,
					sizeof(uint64) * state->ninner_columns);
			}
		}
		if (count > 0)
			pg_batch_shared_spill_write_dense(writer, partition, count,
				out_hashes, out_values, PG_BATCH_SPILL_BLOCK_ROWS,
				out_validity);
	}
	pfree(out_values);
	pfree(out_validity);
}

static void
detect_build_duplicate(BatchHashJoinState *state, ParallelBuildChunk *chunk,
					   uint32 row)
{
	ParallelHashJoinLocal *local = state->parallel;
	ParallelHashShared *shared = local->shared;
	uint32		hash = chunk_hashes(chunk)[row];
	uint32		link = chunk_links(chunk)[row];
	int32	   *values = chunk_values(chunk, state->ninner_columns);

	if (pg_atomic_read_u32(&shared->has_duplicates))
		return;
	while (link != 0)
	{
		uint32		offset;
		ParallelBuildChunk *other = row_chunk(state, link - 1, &offset);
		bool		matches = chunk_hashes(other)[offset] == hash;
		int32	   *other_values = chunk_values(other, state->ninner_columns);

		/*
		 * The int4 hash and the final hash-combine step are reversible, so an
		 * equal combined hash proves equality of the final key. Compare only
		 * the preceding keys. See build_keys_equal() in hash_join.c.
		 */
		for (int key = 0; matches && key < state->nkeys - 1; key++)
		{
			int			column = state->inner_keys[key];

			matches = values[column * PARALLEL_BUILD_CHUNK_ROWS + row] ==
				other_values[column * PARALLEL_BUILD_CHUNK_ROWS + offset];
		}
		if (matches)
		{
			pg_atomic_write_u32(&shared->has_duplicates, 1);
			return;
		}
		link = chunk_links(other)[offset];
	}
}

static void
build_or_spill_chunks(BatchHashJoinState *state)
{
	ParallelHashJoinLocal *local = state->parallel;
	ParallelHashShared *shared = local->shared;
	dsa_pointer *directory = DsaPointerIsValid(shared->directory) ?
		dsa_get_address(local->area, shared->directory) : NULL;
	bool		spilled = pg_atomic_read_u32(&shared->spilled) != 0;

	for (;;)
	{
		uint32		index = pg_atomic_fetch_add_u32(&shared->next_chunk, 1);
		ParallelBuildChunk *chunk;

		if (index >= shared->nchunks)
			break;
		chunk = dsa_get_address(local->area, directory[index]);
		if (spilled)
		{
			spill_chunk(state, chunk);
			continue;
		}
		for (uint32 row = 0; row < chunk->nrows; row++)
		{
			uint32		row_id = chunk->index * PARALLEL_BUILD_CHUNK_ROWS + row;
			uint32		bucket = chunk_hashes(chunk)[row] &
				(shared->nbuckets - 1);
			uint32		head = pg_atomic_read_u32(&local->buckets[bucket]);

			do
				chunk_links(chunk)[row] = head;
			while (!pg_atomic_compare_exchange_u32(&local->buckets[bucket],
											 &head, row_id + 1));
			detect_build_duplicate(state, chunk, row);
		}
	}
	if (spilled)
	{
		get_writer(state, true);
		pg_batch_shared_spill_writer_finish(local->build_writer);
	}
}

static void
partition_probe(BatchHashJoinState *state)
{
	ParallelHashJoinLocal *local = state->parallel;
	ParallelHashShared *shared = local->shared;

	for (;;)
	{
		PgBatchInputResult input;
		PgBatch   *batch;
		InputColumn *columns;
		uint32		hashes[PG_BATCH_SIZE];
		uint64		rows;

		if (!pg_batch_input_next(state->outer_input, &input))
			break;
		batch = input.batch;
		state->probe_batches++;
		state->probe_rows_seen += pg_batch_row_count(batch);
		columns = MemoryContextAllocZero(state->join_context,
			sizeof(InputColumn) * state->nouter_columns);
		for (int key = 0; key < state->nkeys; key++)
		{
			int			column = state->outer_keys[key];

			hash_join_load_input_column(batch,
				state->outer_batch_columns[column], &batch->selection,
				PG_BATCH_FILTER_PHASE, &columns[column], state->join_context);
		}
		rows = hash_batch(state, batch, columns, state->outer_keys, hashes);
		for (uint64 pending = rows; pending != 0; pending &= pending - 1)
		{
			int			row = pg_rightmost_one_pos64(pending);
			int			partition = hashes[row] & (shared->npartitions - 1);

			if (pg_batch_shared_spill_partition_rows(local->build_spill,
					partition) == 0)
				rows &= ~(UINT64CONST(1) << row);
		}
		if (rows != 0)
		{
			PgBatchSelection selected = batch->selection;

			selected.words = &rows;
			for (int column = 0; column < state->nouter_columns; column++)
				if (!columns[column].prepared)
					hash_join_load_input_column(batch,
						state->outer_batch_columns[column], &selected,
						PG_BATCH_PROJECT_PHASE, &columns[column],
						state->join_context);
			spill_vectors(state, batch, columns, state->nouter_columns,
				hashes, rows, false);
		}
		pfree(columns);
		pg_batch_input_finish(state->outer_input);
	}
	get_writer(state, false);
	pg_batch_shared_spill_writer_finish(local->probe_writer);
}

void
parallel_hash_join_prepare(BatchHashJoinState *state)
{
	ParallelHashJoinLocal *local = state->parallel;
	ParallelHashShared *shared;
	int			phase;

	if (local == NULL || local->shared == NULL)
		return;
	shared = local->shared;
	local->area = state->css.ss.ps.state->es_query_dsa;
	if (local->area == NULL)
		elog(ERROR, "pg_batch parallel hash join has no query DSA");
	if (!local->attached)
	{
		BarrierAttach(&shared->build_barrier);
		local->attached = true;
	}
	for (;;)
	{
		phase = BarrierPhase(&shared->build_barrier);
		switch (phase)
		{
			case 0:
				collect_build(state);
				if (BarrierArriveAndWait(&shared->build_barrier, 0))
					setup_shared_hash(state);
				break;
			case 1:
				BarrierArriveAndWait(&shared->build_barrier, 0);
				local->directory = DsaPointerIsValid(shared->directory) ?
					dsa_get_address(local->area, shared->directory) : NULL;
				local->buckets = DsaPointerIsValid(shared->buckets) ?
					dsa_get_address(local->area, shared->buckets) : NULL;
				break;
			case 2:
				build_or_spill_chunks(state);
				BarrierArriveAndWait(&shared->build_barrier, 0);
				break;
			case 3:
				if (!pg_atomic_read_u32(&shared->spilled))
					goto ready;
				partition_probe(state);
				BarrierArriveAndWait(&shared->build_barrier, 0);
				break;
			default:
				goto ready;
		}
	}

ready:
	state->spilled = pg_atomic_read_u32(&shared->spilled) != 0;
	state->npartitions = state->spilled ? shared->npartitions : 0;
	state->build_rows = pg_atomic_read_u64(&shared->build_rows);
	local->explain_build_rows = state->build_rows;
	local->explain_buckets = shared->nbuckets;
	local->explain_chunks = shared->nchunks;
	local->explain_shared_memory = pg_atomic_read_u64(
		&shared->allocated_bytes);
	local->explain_partitions = shared->npartitions;
	local->explain_spilled = state->spilled;
	local->build_has_duplicates =
		pg_atomic_read_u32(&shared->has_duplicates) != 0;
	state->built = true;
	local->current_partition = -1;
	if (state->spilled)
	{
		PgBatchSpillStats build_stats;
		PgBatchSpillStats probe_stats;
		uint64		per_row = Max((uint64) 1,
			(uint64) state->ninner_columns * sizeof(int32) +
			(uint64) sizeof(BuildLink) + 2);

		local->build_chunk_rows = Max((uint32) PG_BATCH_SPILL_BLOCK_ROWS,
			(uint32) Min((uint64) UINT32_MAX,
				(state->memory_limit / per_row) /
				PG_BATCH_SPILL_BLOCK_ROWS * PG_BATCH_SPILL_BLOCK_ROWS));
		hash_join_init_spill_block(&state->probe_block,
			state->nouter_columns, PG_BATCH_SPILL_BLOCK_ROWS,
			state->spill_context, false);
		pg_batch_shared_spill_stats(local->build_spill, &build_stats);
		pg_batch_shared_spill_stats(local->probe_spill, &probe_stats);
		state->spill_build_rows = build_stats.rows_written;
		state->spill_probe_rows = probe_stats.rows_written;
		state->spill_bytes = build_stats.bytes_written +
			probe_stats.bytes_written;
		state->spill_pages_written = build_stats.blocks_written +
			probe_stats.blocks_written;
	}
}

static ParallelBuildChunk *
row_chunk(BatchHashJoinState *state, uint32 row, uint32 *offset)
{
	ParallelHashJoinLocal *local = state->parallel;
	uint32		index = row / PARALLEL_BUILD_CHUNK_ROWS;

	if (index >= local->shared->nchunks)
		elog(ERROR, "invalid pg_batch shared hash row");
	*offset = row % PARALLEL_BUILD_CHUNK_ROWS;
	return dsa_get_address(local->area, local->directory[index]);
}

uint32
parallel_hash_join_find_head(BatchHashJoinState *state, uint32 hash,
							 int probe_row)
{
	ParallelHashJoinLocal *local = state->parallel;

	if (local->local_build)
	{
		BuildStore *store = &state->build;
		uint32		bucket = hash & (store->nbuckets - 1);

		for (;;)
		{
			uint32		link = store->buckets[bucket];
			bool		matches = link != 0 &&
				store->links[link - 1].hash == hash;

			for (int key = 0; matches && key < state->nkeys - 1; key++)
			{
				bool		isnull;
				int32		probe_value = hash_join_input_value(
					&state->probe_columns[state->outer_keys[key]], probe_row,
					&isnull);

				matches = !isnull && store->columns[state->inner_keys[key]].
					values[link - 1] == probe_value;
			}
			if (matches || link == 0)
				return link;
			bucket = (bucket + 1) & (store->nbuckets - 1);
		}
	}
	else
	{
		uint32		link = pg_atomic_read_u32(
			&local->buckets[hash & (local->shared->nbuckets - 1)]);

		while (link != 0)
		{
			uint32		offset;
			ParallelBuildChunk *chunk = row_chunk(state, link - 1, &offset);
			bool		matches = chunk_hashes(chunk)[offset] == hash;
			int32	   *values = chunk_values(chunk, state->ninner_columns);

			for (int key = 0; matches && key < state->nkeys - 1; key++)
			{
				bool		isnull;
				int32		probe_value = hash_join_input_value(
					&state->probe_columns[state->outer_keys[key]], probe_row,
					&isnull);

				matches = !isnull &&
					values[state->inner_keys[key] *
						PARALLEL_BUILD_CHUNK_ROWS + offset] == probe_value;
			}
			if (matches)
				return link;
			link = chunk_links(chunk)[offset];
		}
		return 0;
	}
}

uint32
parallel_hash_join_next(BatchHashJoinState *state, uint32 build_row,
						int probe_row)
{
	ParallelHashJoinLocal *local = state->parallel;

	if (local->local_build)
		return state->build.links[build_row].next;
	else
	{
		uint32		offset;
		ParallelBuildChunk *chunk = row_chunk(state, build_row, &offset);
		uint32		hash = chunk_hashes(chunk)[offset];
		uint32		link = chunk_links(chunk)[offset];

		/* Shared buckets contain hash collisions as well as true duplicates. */
		while (link != 0)
		{
			int32	   *values;
			bool		matches;

			chunk = row_chunk(state, link - 1, &offset);
			matches = chunk_hashes(chunk)[offset] == hash;
			values = chunk_values(chunk, state->ninner_columns);
			for (int key = 0; matches && key < state->nkeys - 1; key++)
			{
				bool		isnull;
				int32		probe_value = hash_join_input_value(
					&state->probe_columns[state->outer_keys[key]], probe_row,
					&isnull);

				matches = !isnull &&
					values[state->inner_keys[key] *
						PARALLEL_BUILD_CHUNK_ROWS + offset] == probe_value;
			}
			if (matches)
				return link;
			link = chunk_links(chunk)[offset];
		}
		return 0;
	}
}

bool
parallel_hash_join_build_is_unique(BatchHashJoinState *state)
{
	ParallelHashJoinLocal *local = state->parallel;

	Assert(local != NULL);
	if (local->local_build)
		return !state->build.has_duplicates;
	Assert(!state->spilled);
	return !local->build_has_duplicates;
}

int32
parallel_hash_join_build_value(BatchHashJoinState *state, int column,
							   uint32 row, bool *isnull)
{
	ParallelHashJoinLocal *local = state->parallel;

	if (local->local_build)
	{
		*isnull = (state->build.columns[column].validity[row / 64] &
			(UINT64CONST(1) << (row % 64))) == 0;
		return *isnull ? 0 : state->build.columns[column].values[row];
	}
	else
	{
		uint32		offset;
		ParallelBuildChunk *chunk = row_chunk(state, row, &offset);
		uint64	   *validity = chunk_validity(chunk);
		int32	   *values = chunk_values(chunk, state->ninner_columns);

		*isnull = (validity[column * (PARALLEL_BUILD_CHUNK_ROWS / 64) +
			offset / 64] & (UINT64CONST(1) << (offset % 64))) == 0;
		return *isnull ? 0 :
			values[column * PARALLEL_BUILD_CHUNK_ROWS + offset];
	}
}

static void
close_partition(ParallelHashJoinLocal *local)
{
	pg_batch_shared_spill_reader_close(local->build_reader);
	pg_batch_shared_spill_reader_close(local->probe_reader);
	pg_batch_spill_reader_close(local->repartition_build_reader);
	pg_batch_spill_reader_close(local->repartition_probe_reader);
	local->build_reader = NULL;
	local->probe_reader = NULL;
	local->repartition_build_reader = NULL;
	local->repartition_probe_reader = NULL;
	local->local_build = false;
}

static void
destroy_repartition(ParallelHashJoinLocal *local)
{
	close_partition(local);
	pg_batch_spill_destroy(local->repartition_build);
	pg_batch_spill_destroy(local->repartition_probe);
	local->repartition_build = NULL;
	local->repartition_probe = NULL;
	local->current_repartition = -1;
	local->nrepartitions = 0;
}

static bool
next_build_block(ParallelHashJoinLocal *local, PgBatchSpillBlock *block)
{
	if (local->repartition_build_reader != NULL)
		return pg_batch_spill_reader_next(local->repartition_build_reader,
			block);
	return pg_batch_shared_spill_reader_next(local->build_reader, block);
}

static bool
next_probe_block(ParallelHashJoinLocal *local, PgBatchSpillBlock *block)
{
	if (local->repartition_probe_reader != NULL)
		return pg_batch_spill_reader_next(local->repartition_probe_reader,
			block);
	return pg_batch_shared_spill_reader_next(local->probe_reader, block);
}

static void
rewind_probe(ParallelHashJoinLocal *local)
{
	if (local->repartition_probe_reader != NULL)
		pg_batch_spill_reader_rewind(local->repartition_probe_reader);
	else
		pg_batch_shared_spill_reader_rewind(local->probe_reader);
}

static bool
load_local_build_chunk(BatchHashJoinState *state)
{
	ParallelHashJoinLocal *local = state->parallel;
	PgBatchSpillBlock block;

	hash_join_reset_build_store(&state->build);
	while (state->build.nrows < local->build_chunk_rows &&
		   next_build_block(local, &block))
		hash_join_append_dense_build(state, &block);
	if (state->build.nrows == 0)
		return false;
	hash_join_finish_build_table(state);
	local->local_build = true;
	local->build_maybe_more = state->build.nrows >= local->build_chunk_rows;
	return true;
}

static int
partition_bits(int partitions)
{
	int			bits = 0;

	while ((1 << bits) < partitions)
		bits++;
	return bits;
}

static void
write_repartition_block(PgBatchSpillSet *set,
						const PgBatchSpillBlock *block, int shift,
						int partitions)
{
	PgBatchInt4Vector *columns = palloc_array(PgBatchInt4Vector,
		block->ncolumns);
	uint64	   *rows = palloc0_array(uint64, partitions);

	for (int column = 0; column < block->ncolumns; column++)
		pg_batch_int4_vector_init_packed(&columns[column],
			&block->values[column * PG_BATCH_SPILL_BLOCK_ROWS],
			&block->validity[column * 8], 0);
	for (int row = 0; row < block->nrows; row++)
	{
		int			partition = (block->hashes[row] >> shift) &
			(partitions - 1);

		if ((block->selection & (UINT64CONST(1) << row)) != 0)
			rows[partition] |= UINT64CONST(1) << row;
	}
	for (int partition = 0; partition < partitions; partition++)
	{
		PgBatchSelection selected;

		if (rows[partition] == 0)
			continue;
		selected.nrows = block->nrows;
		selected.nwords = 1;
		selected.words = &rows[partition];
		pg_batch_spill_write(set, partition, columns, &selected,
			block->hashes);
	}
	pfree(rows);
	pfree(columns);
}

static void
record_repartition_writes(BatchHashJoinState *state, PgBatchSpillSet *set)
{
	const PgBatchSpillStats *stats = pg_batch_spill_stats(set);

	state->spill_bytes += stats->bytes_written;
	state->spill_pages_written += stats->blocks_written;
}

static void
repartition_shared_partition(BatchHashJoinState *state, int partition,
								uint64 build_rows)
{
	ParallelHashJoinLocal *local = state->parallel;
	ParallelHashShared *shared = local->shared;
	uint64		passes = (build_rows + local->build_chunk_rows - 1) /
		local->build_chunk_rows;
	TupleDesc	build_desc;
	TupleDesc	probe_desc;
	PgBatchSpillBlock block;

	local->nrepartitions = next_partition_count(passes);
	local->current_repartition = -1;
	local->repartition_shift = partition_bits(shared->npartitions);
	build_desc = int4_tuple_desc(state->ninner_columns);
	probe_desc = int4_tuple_desc(state->nouter_columns);
	local->repartition_build = pg_batch_spill_create(state->spill_context,
		build_desc, state->ninner_columns, local->nrepartitions,
		state->memory_limit / 4);
	local->repartition_probe = pg_batch_spill_create(state->spill_context,
		probe_desc, state->nouter_columns, local->nrepartitions,
		state->memory_limit / 4);
	FreeTupleDesc(build_desc);
	FreeTupleDesc(probe_desc);
	local->build_reader = pg_batch_shared_spill_reader_open(
		state->spill_context, local->build_spill, partition);
	while (pg_batch_shared_spill_reader_next(local->build_reader, &block))
		write_repartition_block(local->repartition_build, &block,
			local->repartition_shift, local->nrepartitions);
	pg_batch_shared_spill_reader_close(local->build_reader);
	local->build_reader = NULL;
	pg_batch_spill_finish(local->repartition_build, false);
	local->probe_reader = pg_batch_shared_spill_reader_open(
		state->spill_context, local->probe_spill, partition);
	while (pg_batch_shared_spill_reader_next(local->probe_reader, &block))
	{
		uint64		rows = block.selection;

		/* Do not write probe rows for empty second-level partitions. */
		for (int row = 0; row < block.nrows; row++)
		{
			int			subpartition =
				(block.hashes[row] >> local->repartition_shift) &
				(local->nrepartitions - 1);

			if (pg_batch_spill_partition_empty(local->repartition_build,
					subpartition))
				rows &= ~(UINT64CONST(1) << row);
		}
		if (rows != 0)
		{
			PgBatchSpillBlock selected = block;

			selected.selection = rows;
			write_repartition_block(local->repartition_probe, &selected,
				local->repartition_shift, local->nrepartitions);
		}
	}
	pg_batch_shared_spill_reader_close(local->probe_reader);
	local->probe_reader = NULL;
	pg_batch_spill_finish(local->repartition_probe, true);
	record_repartition_writes(state, local->repartition_build);
	record_repartition_writes(state, local->repartition_probe);
	state->spill_repartitions++;
}

static bool
start_next_repartition(BatchHashJoinState *state)
{
	ParallelHashJoinLocal *local = state->parallel;

	close_partition(local);
	while (++local->current_repartition < local->nrepartitions)
	{
		int			partition = local->current_repartition;

		if (pg_batch_spill_partition_empty(local->repartition_build,
				partition) ||
			pg_batch_spill_partition_empty(local->repartition_probe,
				partition))
			continue;
		local->repartition_build_reader = pg_batch_spill_reader_open(
			local->repartition_build, partition);
		local->repartition_probe_reader = pg_batch_spill_reader_open(
			local->repartition_probe, partition);
		if (load_local_build_chunk(state))
			return true;
		close_partition(local);
	}
	destroy_repartition(local);
	return false;
}

static bool
start_next_partition(BatchHashJoinState *state)
{
	ParallelHashJoinLocal *local = state->parallel;
	ParallelHashShared *shared = local->shared;

	close_partition(local);
	for (;;)
	{
		uint32		partition = pg_atomic_fetch_add_u32(
			&shared->next_partition, 1);
		uint64		build_rows;

		if (partition >= (uint32) shared->npartitions)
			return false;
		build_rows = pg_batch_shared_spill_partition_rows(
			local->build_spill, partition);
		if (build_rows == 0 ||
			pg_batch_shared_spill_partition_rows(local->probe_spill,
				partition) == 0)
			continue;
		local->current_partition = partition;
		if ((build_rows + local->build_chunk_rows - 1) /
			local->build_chunk_rows >= PARALLEL_REPARTITION_MIN_PASSES)
		{
			repartition_shared_partition(state, partition, build_rows);
			if (start_next_repartition(state))
				return true;
			continue;
		}
		local->build_reader = pg_batch_shared_spill_reader_open(
			state->spill_context, local->build_spill, partition);
		local->probe_reader = pg_batch_shared_spill_reader_open(
			state->spill_context, local->probe_spill, partition);
		if (!load_local_build_chunk(state))
		{
			close_partition(local);
			continue;
		}
		return true;
	}
}

bool
parallel_hash_join_fetch_probe(BatchHashJoinState *state)
{
	ParallelHashJoinLocal *local = state->parallel;

	if (!state->spilled)
	{
		PgBatchInputResult input;

		if (!pg_batch_input_next(state->outer_input, &input))
			return false;
		state->probe_batch = input.batch;
		state->probe_from_spill = false;
		state->probe_batches++;
		state->probe_rows_seen += pg_batch_row_count(state->probe_batch);
		MemSet(state->probe_columns, 0,
			sizeof(InputColumn) * state->nouter_columns);
		state->next_probe_row = -1;
		hash_join_prepare_probe_keys(state);
		state->probe_hash_rows = hash_batch(state, state->probe_batch,
			state->probe_columns, state->outer_keys, state->probe_hashes);
		return true;
	}
	for (;;)
	{
		PgBatchSpillBlock block;

		if (local->probe_reader == NULL &&
			local->repartition_probe_reader == NULL &&
			!start_next_partition(state))
			return false;
		if (next_probe_block(local, &block))
		{
			hash_join_use_spill_block(state, &state->probe_block, &block);
			state->probe_batch = &state->probe_block.batch;
			state->probe_from_spill = true;
			state->spill_probe_reads += block.nrows;
			MemSet(state->probe_columns, 0,
				sizeof(InputColumn) * state->nouter_columns);
			state->next_probe_row = -1;
			hash_join_prepare_probe_keys(state);
			return true;
		}
		if (local->build_maybe_more && load_local_build_chunk(state))
		{
			rewind_probe(local);
			continue;
		}
		if (local->repartition_probe_reader != NULL &&
			start_next_repartition(state))
			continue;
		if (!start_next_partition(state))
			return false;
	}
}

void
parallel_hash_join_finish_probe(BatchHashJoinState *state)
{
	if (state->probe_batch == NULL)
		return;
	if (!state->spilled)
		pg_batch_input_finish(state->outer_input);
	state->probe_batch = NULL;
}

static void
release_local_resources(ParallelHashJoinLocal *local)
{
	destroy_repartition(local);
	pg_batch_shared_spill_writer_destroy(local->build_writer);
	pg_batch_shared_spill_writer_destroy(local->probe_writer);
	local->build_writer = NULL;
	local->probe_writer = NULL;
}

void
parallel_hash_join_shutdown(CustomScanState *node)
{
	BatchHashJoinState *state = (BatchHashJoinState *) node;
	ParallelHashJoinLocal *local = state->parallel;

	if (local == NULL)
		return;
	/* Shutdown runs while DSM and its SharedFileSets are still mapped. */
	release_local_resources(local);
	if (local->attached)
	{
		BarrierDetach(&local->shared->build_barrier);
		local->attached = false;
	}
}

void
parallel_hash_join_end(BatchHashJoinState *state)
{
	ParallelHashJoinLocal *local = state->parallel;

	if (local == NULL)
		return;
	/* Normally ShutdownCustomScan already released every DSM-backed object. */
	if (local->attached)
	{
		release_local_resources(local);
		BarrierDetach(&local->shared->build_barrier);
		local->attached = false;
	}
}

void
parallel_hash_join_rescan(BatchHashJoinState *state)
{
	ParallelHashJoinLocal *local = state->parallel;

	if (local == NULL)
		return;
	release_local_resources(local);
	local->chunk = NULL;
	local->chunk_pointer = InvalidDsaPointer;
	local->directory = NULL;
	local->buckets = NULL;
	local->current_partition = -1;
	if (local->attached)
	{
		BarrierDetach(&local->shared->build_barrier);
		local->attached = false;
	}
}

void
parallel_hash_join_explain(BatchHashJoinState *state, ExplainState *es)
{
	ParallelHashJoinLocal *local = state->parallel;

	if (local == NULL)
		return;
	ExplainPropertyBool("Shared Hash", !local->explain_spilled, es);
	ExplainPropertyInteger("Shared Build Rows", NULL,
		local->explain_build_rows, es);
	if (!local->explain_spilled)
	{
		ExplainPropertyBool("Build Keys Unique",
			!local->build_has_duplicates, es);
		ExplainPropertyInteger("Shared Hash Buckets", NULL,
			local->explain_buckets, es);
		ExplainPropertyInteger("Shared Build Chunks", NULL,
			local->explain_chunks, es);
		ExplainPropertyInteger("Shared Hash Memory", "kB",
			local->explain_shared_memory / 1024, es);
	}
	else
	{
		ExplainPropertyInteger("Shared Spill Partitions", NULL,
			local->explain_partitions, es);
		if (es->analyze)
		{
			ExplainPropertyInteger("Spill Bytes Written", "kB",
				state->spill_bytes / 1024, es);
			ExplainPropertyInteger("Spill Pages Written", NULL,
				state->spill_pages_written, es);
			ExplainPropertyInteger("Repartitioned Spill Partitions", NULL,
				state->spill_repartitions, es);
		}
	}
}
