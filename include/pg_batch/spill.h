/*
 * Buffered temporary storage for int4 batch operators.
 *
 * Spill sets own a fixed number of logical-tape partitions. Rows are written
 * from PgBatchInt4Vector views and read back as dense column-major blocks.
 * The caller chooses partitioning and any recursive processing policy.
 */
#ifndef PG_BATCH_SPILL_H
#define PG_BATCH_SPILL_H

#include "postgres.h"

#include "access/tupdesc.h"
#include "storage/dsm.h"

#include "batch.h"
#include "vector.h"

#define PG_BATCH_SPILL_BLOCK_ROWS 64

typedef struct PgBatchSpillSet PgBatchSpillSet;
typedef struct PgBatchSpillReader PgBatchSpillReader;
typedef struct PgBatchSharedSpill PgBatchSharedSpill;
typedef struct PgBatchSharedSpillWriter PgBatchSharedSpillWriter;
typedef struct PgBatchSharedSpillReader PgBatchSharedSpillReader;

typedef struct PgBatchSpillStats
{
	uint64		rows_written;
	uint64		rows_read;
	uint64		blocks_written;
	uint64		blocks_read;
	uint64		bytes_written;
	uint64		bytes_read;
} PgBatchSpillStats;

/*
 * Borrowed dense block, valid until the reader's next operation. Values are
 * column-major with PG_BATCH_SPILL_BLOCK_ROWS entries per column. Selection
 * contains the first nrows bits and validity has one 8-byte bitmap per column.
 */
typedef struct PgBatchSpillBlock
{
	int			nrows;
	int			ncolumns;
	uint64		selection;
	const uint32 *hashes;
	const int32 *values;
	/* One 64-row validity bitmap (8 bytes) per column. */
	const uint8 *validity;
} PgBatchSpillBlock;

/*
 * Create npartitions empty tapes. The first ncolumns descriptor attributes
 * must be live int4 columns; the descriptor is validated but not retained.
 * buffer_limit bounds the memory used by pending 64-row writer blocks. At
 * least one block is kept even when the limit is smaller than one block.
 */
extern PgBatchSpillSet *pg_batch_spill_create(
	MemoryContext parent_context, TupleDesc tuple_desc,
	int ncolumns, int npartitions, Size buffer_limit);

/* Append selected vector rows to one partition, densely and in row order. */
extern void pg_batch_spill_write(PgBatchSpillSet *set, int partition,
	const PgBatchInt4Vector *columns, const PgBatchSelection *selection,
	const uint32 *hashes);

/* Append an already dense block; values use value_stride rows per column. */
extern void pg_batch_spill_write_dense(PgBatchSpillSet *set, int partition,
	int nrows, const uint32 *hashes, const int32 *values,
	int value_stride, const uint64 *validity);

/*
 * Flush all writers and make partitions readable. Frozen sets support reader
 * rewind; ordinary sets use destructive reads and release disk blocks early.
 */
extern void pg_batch_spill_finish(PgBatchSpillSet *set, bool frozen);

extern bool pg_batch_spill_partition_empty(const PgBatchSpillSet *set,
	int partition);
extern uint64 pg_batch_spill_partition_rows(const PgBatchSpillSet *set,
	int partition);

/* Open one reader after finish(). Empty partitions return a valid EOF reader. */
extern PgBatchSpillReader *pg_batch_spill_reader_open(
	PgBatchSpillSet *set, int partition);
extern bool pg_batch_spill_reader_next(PgBatchSpillReader *reader,
	PgBatchSpillBlock *result);
extern void pg_batch_spill_reader_rewind(PgBatchSpillReader *reader);
extern void pg_batch_spill_reader_close(PgBatchSpillReader *reader);

extern const PgBatchSpillStats *pg_batch_spill_stats(
	const PgBatchSpillSet *set);
extern void pg_batch_spill_destroy(PgBatchSpillSet *set);

/*
 * Shared spill storage for parallel nodes.
 *
 * The caller places pg_batch_shared_spill_estimate() bytes in DSM and owns
 * synchronization between its write and read phases. Each participant has
 * one writer and writes only its own files. A partition reader combines the
 * finished files from all participants. The shared object owns a
 * SharedFileSet whose lifetime is tied to the supplied DSM segment.
 */
extern Size pg_batch_shared_spill_estimate(int participants, int partitions);
extern PgBatchSharedSpill *pg_batch_shared_spill_initialize(
	void *shared_memory, dsm_segment *segment, int participants,
	int partitions, int ncolumns);
extern PgBatchSharedSpill *pg_batch_shared_spill_attach(
	void *shared_memory, dsm_segment *segment);
extern void pg_batch_shared_spill_reset(PgBatchSharedSpill *shared);

extern PgBatchSharedSpillWriter *pg_batch_shared_spill_writer_create(
	MemoryContext parent_context, PgBatchSharedSpill *shared,
	int participant, TupleDesc tuple_desc, Size buffer_limit);
extern void pg_batch_shared_spill_write(PgBatchSharedSpillWriter *writer,
	int partition, const PgBatchInt4Vector *columns,
	const PgBatchSelection *selection, const uint32 *hashes);
extern void pg_batch_shared_spill_write_dense(
	PgBatchSharedSpillWriter *writer, int partition, int nrows,
	const uint32 *hashes, const int32 *values, int value_stride,
	const uint64 *validity);
extern void pg_batch_shared_spill_writer_finish(
	PgBatchSharedSpillWriter *writer);
extern void pg_batch_shared_spill_writer_destroy(
	PgBatchSharedSpillWriter *writer);

extern uint64 pg_batch_shared_spill_partition_rows(
	const PgBatchSharedSpill *shared, int partition);
extern PgBatchSharedSpillReader *pg_batch_shared_spill_reader_open(
	MemoryContext parent_context, PgBatchSharedSpill *shared, int partition);
extern bool pg_batch_shared_spill_reader_next(
	PgBatchSharedSpillReader *reader, PgBatchSpillBlock *result);
extern void pg_batch_shared_spill_reader_rewind(
	PgBatchSharedSpillReader *reader);
extern void pg_batch_shared_spill_reader_close(
	PgBatchSharedSpillReader *reader);
extern const PgBatchSpillStats *pg_batch_shared_spill_reader_stats(
	const PgBatchSharedSpillReader *reader);
extern void pg_batch_shared_spill_stats(const PgBatchSharedSpill *shared,
	PgBatchSpillStats *result);

#endif /* PG_BATCH_SPILL_H */
