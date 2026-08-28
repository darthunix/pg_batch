#ifndef PG_BATCH_HASH_JOIN_INTERNAL_H
#define PG_BATCH_HASH_JOIN_INTERNAL_H

#include "executor/nodeCustom.h"
#include "storage/buffile.h"

#include "internal.h"

/*
 * Private interface shared by the hash join executor and its spill module.
 * None of these types or functions are part of the bridge ABI.
 */

typedef struct InputColumn
{
	bool		prepared;
	PgBatchInt4Vector vector;
} InputColumn;

typedef struct StoredColumn
{
	int32	   *values;
	uint64	   *validity;
} StoredColumn;

typedef struct BuildLink
{
	uint32		hash;
	uint32		next;
} BuildLink;

typedef struct BuildStore
{
	MemoryContext context;
	StoredColumn *columns;
	BuildLink  *links;
	uint32	   *buckets;
	int			ncolumns;
	uint32		nrows;
	uint32		capacity;
	uint32		nbuckets;
	int			hash_shift;
	bool		has_duplicates;
} BuildStore;

/*
 * Columnar rows used by spill input and by the dense resident-probe buffer.
 * Disk I/O still reads and writes PG_BATCH_SIZE rows at a time; capacity is
 * larger only for the in-memory buffer that bridges adjacent input batches.
 */
typedef struct SpillBlock
{
	PgBatchBridgeBatch batch;
	uint64		selection;
	int			ncolumns;
	int			nrows;
	int			capacity;
	uint32	   *hashes;
	int32	   *values;
	uint8	   *validity;
	const void **buffers;
	struct ArrowArray *arrays;
	struct ArrowSchema *schemas;
} SpillBlock;

typedef struct OutputColumn
{
	/* NULL for a direct raw column; otherwise evaluate against raw_slot. */
	ExprState  *expr;
	Bitmapset  *raw_columns;
	int			direct_raw_column;
	int32		values[PG_BATCH_SIZE];
	uint8		validity[PG_BATCH_SIZE / 8];
	Datum		datums[PG_BATCH_SIZE];
	bool		isnull[PG_BATCH_SIZE];
	uint64		native_rows;
	uint64		datum_rows;
	int64		null_count;
	const void *buffers[2];
	struct ArrowArray array;
	struct ArrowSchema schema;
} OutputColumn;

typedef struct BatchJoinQual
{
	FmgrInfo	func;
	FunctionCallInfo fcinfo;
	Oid			funcid;
	int			left_column;
	int			right_column;
} BatchJoinQual;

typedef struct BatchHashJoinState
{
	/* Plan children, slots, and mappings from plan columns to batch columns. */
	CustomScanState css;
	PlanState  *outer_plan;
	PlanState  *inner_plan;
	const PgBatchBridgeProducerOps *outer_producer;
	const PgBatchBridgeProducerOps *inner_producer;
	PgBatchBridgeBinding *outer_request_binding;
	PgBatchBridgeBinding *inner_request_binding;
	TupleTableSlot *outer_result_slot;
	TupleTableSlot *inner_result_slot;
	PgBatchBridgeBinding *outer_result_binding;
	PgBatchBridgeBinding *inner_result_binding;
	TupleTableSlot *output_slot;
	TupleTableSlot *raw_slot;
	const PgBatchBridgeRequest *output_request;

	int			nouter_columns;
	int			ninner_columns;
	int			nkeys;
	int			noutput_columns;
	int		   *outer_batch_columns;
	int		   *inner_batch_columns;
	int		   *outer_keys;
	int		   *inner_keys;
	Bitmapset  *outer_key_columns;
	Bitmapset  *spill_outer_key_columns;
	Bitmapset  *qual_raw_columns;
	Bitmapset  *qual_probe_columns;
	BatchJoinQual *batch_quals;
	int			nbatch_quals;
	MemoryContext qual_context;

	/* Build table and all state owned by the private spill implementation. */
	MemoryContext join_context;
	MemoryContext build_context;
	MemoryContext spill_context;
	MemoryContext slot_context;
	BuildStore	build;
	bool		built;
	size_t		memory_limit;
	int			planned_partitions;
	bool		spilled;
	int			npartitions;
	int			partition_shift;

	/* Build partitions kept in memory while the others use spill files. */
	Bitmapset  *resident_partitions;
	uint64	   *resident_partition_rows;
	uint64		resident_build_rows;
	uint64		resident_probe_rows;
	uint64		resident_memory_limit;

	/* Complete build-key filter used before probe payload materialization. */
	uint64	   *bloom_words;
	uint64		bloom_nbits;
	uint64		bloom_bytes;
	bool		bloom_usable;
	uint64		bloom_rejected_rows;

	/* The probe first streams resident rows, then restores spilled rows. */
	bool		probe_input_done;
	bool		probe_from_spill;
	bool		spill_probe_started;
	int			current_partition;
	BufFile   **build_files;
	BufFile   **probe_files;
	uint64	   *build_partition_rows;
	BufFile    *active_build_file;
	BufFile    *active_probe_file;
	int			active_hash_shift;
	BufFile   **repartition_build_files;
	BufFile   **repartition_probe_files;
	uint64	   *repartition_build_rows;
	int			nrepartitions;
	int			current_repartition;
	int			repartition_hash_shift;
	int		   *spill_partition_heads;
	int		   *spill_partition_tails;
	bool		spill_buffered;
	int			spill_buffer_columns;
	int		   *spill_buffer_counts;
	uint32	   *spill_buffer_hashes;
	int32	   *spill_buffer_values;
	uint64	   *spill_buffer_validity;
	SpillBlock	build_block;
	SpillBlock	probe_block;
	/* Two batches let resident rows be packed densely across input batches. */
	SpillBlock	resident_probe_block;
	bool		probe_from_resident;
	int			build_block_row;
	bool		build_file_eof;
	bool		build_partition_more;
	bool		partition_active;
	uint32		build_chunk_rows;
	uint64		spill_bytes;
	uint64		spill_build_rows;
	uint64		spill_probe_rows;
	uint64		spill_probe_reads;
	uint64		spill_repartitions;
	uint64		spill_pages_written;
	uint64		spill_pages_read;
	uint64		peak_memory;

	/* Current probe batch and candidate build rows. */
	PgBatchBridgeBatch *probe_batch;
	InputColumn *probe_columns;
	const PgBatchInt4Vector **hash_keys;
	uint32		probe_hashes[PG_BATCH_SIZE];
	uint64		probe_hash_rows;
	int			next_probe_row;
	int			probe_active;
	int			probe_rows[PG_BATCH_SIZE];
	uint32		probe_build_rows[PG_BATCH_SIZE];
	bool		probe_done_pending;

	/* Lazily gathered output batch. */
	PgBatchBridgeBatch output_batch;
	uint64		output_selection;
	int			output_probe_rows[PG_BATCH_SIZE];
	uint32		output_build_rows[PG_BATCH_SIZE];
	OutputColumn *output_columns;
	int			output_count;
	int			output_qualified;
	int			next_output_row;
	bool		output_published;

	/* EXPLAIN ANALYZE counters. */
	uint64		build_batches;
	uint64		build_input_rows;
	uint64		build_rows;
	uint64		probe_batches;
	uint64		probe_rows_seen;
	uint64		output_rows;
} BatchHashJoinState;

/* These helpers stay inline because both build and spill loops call them. */
static inline int32
hash_join_input_value(const InputColumn *input, int row, bool *isnull)
{
	Assert(input->prepared);
	*isnull = pg_batch_int4_row_is_null(&input->vector, row);
	return *isnull ? 0 : pg_batch_int4_row_value(&input->vector, row);
}

static inline int32
hash_join_spill_value(const SpillBlock *block, int column, int row)
{
	return block->values[column * block->capacity + row];
}

static inline bool
hash_join_spill_value_valid(const SpillBlock *block, int column, int row)
{
	int			stride = (block->capacity + 7) / 8;

	return (block->validity[column * stride + row / 8] &
			((uint8) 1 << (row % 8))) != 0;
}

/* Services implemented by hash_join.c and used by the spill subsystem. */
extern void hash_join_load_input_column(PgBatchBridgeBatch *batch, int column,
										const uint64 *rows,
										PgBatchMaterializePhase phase,
										InputColumn *result,
										MemoryContext scratch_context);
extern void hash_join_reset_build_store(BuildStore *store);
extern void hash_join_grow_build_store(BuildStore *store);
extern void hash_join_finish_build_table(BatchHashJoinState *state);
extern void hash_join_track_peak_memory(BatchHashJoinState *state);
extern void hash_join_prepare_probe_keys(BatchHashJoinState *state);

/* Spill lifecycle and input/output services implemented separately. */
extern uint64 hash_join_estimated_build_memory(uint64 nrows, int ncolumns);
extern void hash_join_begin_spill(BatchHashJoinState *state,
								  uint64 estimated_bytes,
								  uint64 estimated_rows);
extern uint64 hash_join_spill_input_batch(BatchHashJoinState *state,
										  PgBatchBridgeBatch *batch,
										  InputColumn *columns, int ncolumns,
										  const uint32 *hashes,
										  uint64 hash_rows,
										  const int *batch_columns,
										  BufFile **files,
										  uint64 *resident_rows);
extern void hash_join_spill_build_store(BatchHashJoinState *state);
extern void hash_join_finish_spilled_build(BatchHashJoinState *state);
extern void hash_join_finish_spilled_probe(BatchHashJoinState *state);
extern bool hash_join_publish_resident_probe(BatchHashJoinState *state,
											 bool final_batch);
extern void hash_join_finish_resident_probe(BatchHashJoinState *state);
extern bool hash_join_fetch_spilled_probe_batch(BatchHashJoinState *state);
extern void hash_join_close_spill_files(BatchHashJoinState *state);

#endif
