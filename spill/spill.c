#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "storage/buffile.h"
#include "storage/sharedfileset.h"
#include "utils/logtape.h"
#include "utils/memutils.h"

#include "spill.h"

#define SPILL_MAGIC 0x50474253U

typedef struct SpillHeader
{
	uint32		magic;
	uint16		nrows;
	uint16		ncolumns;
} SpillHeader;

typedef struct SpillWriter
{
	int			nrows;
	uint32		hashes[PG_BATCH_SPILL_BLOCK_ROWS];
	int32	   *values;
	uint8	   *validity;
} SpillWriter;

struct PgBatchSpillSet
{
	MemoryContext context;
	int			ncolumns;
	int			npartitions;
	LogicalTapeSet *tapeset;
	LogicalTape **tapes;
	SpillWriter **writers;
	uint64	   *partition_rows;
	int			active_writers;
	int			max_writers;
	int			eviction_cursor;
	bool		finished;
	bool		frozen;
	PgBatchSpillStats stats;
};

struct PgBatchSpillReader
{
	PgBatchSpillSet *set;
	LogicalTape *tape;
	int			partition;
	bool		eof;
	uint32		hashes[PG_BATCH_SPILL_BLOCK_ROWS];
	int32	   *values;
	uint8	   *validity;
};

struct PgBatchSharedSpill
{
	int			participants;
	int			partitions;
	int			ncolumns;
	SharedFileSet fileset;
	/* uint64 rows[participants * partitions] follows. */
	/* PgBatchSpillStats stats[participants] follows rows. */
	/* uint8 finished[participants] follows stats. */
};

struct PgBatchSharedSpillWriter
{
	MemoryContext context;
	PgBatchSharedSpill *shared;
	int			participant;
	int			ncolumns;
	BufFile   **files;
	SpillWriter **writers;
	uint64	   *partition_rows;
	int			active_writers;
	int			max_writers;
	int			eviction_cursor;
	bool			finished;
	PgBatchSpillStats stats;
};

struct PgBatchSharedSpillReader
{
	MemoryContext context;
	PgBatchSharedSpill *shared;
	int			partition;
	int			participant;
	BufFile    *file;
	uint32		hashes[PG_BATCH_SPILL_BLOCK_ROWS];
	int32	   *values;
	uint8	   *validity;
	PgBatchSpillStats stats;
};

static uint64 *
shared_rows(const PgBatchSharedSpill *shared)
{
	return (uint64 *) ((char *) shared + MAXALIGN(sizeof(*shared)));
}

static PgBatchSpillStats *
shared_stats(const PgBatchSharedSpill *shared)
{
	return (PgBatchSpillStats *) (shared_rows(shared) +
		shared->participants * shared->partitions);
}

static uint8 *
shared_finished(const PgBatchSharedSpill *shared)
{
	return (uint8 *) (shared_stats(shared) + shared->participants);
}

static void
shared_file_name(char *name, int participant, int partition)
{
	snprintf(name, MAXPGPATH, "worker-%d-partition-%d",
			 participant, partition);
}

static Size
block_bytes(int ncolumns, int nrows)
{
	return sizeof(SpillHeader) + sizeof(uint32) * nrows +
		sizeof(uint64) * ncolumns + sizeof(int32) * ncolumns * nrows;
}

static Size
writer_bytes(int ncolumns)
{
	return add_size(sizeof(SpillWriter),
		add_size(mul_size(sizeof(int32) * PG_BATCH_SPILL_BLOCK_ROWS,
				ncolumns), mul_size(8, ncolumns)));
}

static void
validate_partition(const PgBatchSpillSet *set, int partition)
{
	if (partition < 0 || partition >= set->npartitions)
		elog(ERROR, "pg_batch spill partition %d is out of range", partition);
}

static void flush_writer(PgBatchSpillSet *set, int partition);

static void
free_writer(PgBatchSpillSet *set, int partition)
{
	SpillWriter *writer = set->writers[partition];

	if (writer != NULL)
	{
		pfree(writer->values);
		pfree(writer->validity);
		pfree(writer);
		set->writers[partition] = NULL;
		set->active_writers--;
	}
}

static void
flush_writer(PgBatchSpillSet *set, int partition)
{
	SpillWriter *writer = set->writers[partition];
	SpillHeader header;

	if (writer == NULL || writer->nrows == 0)
		return;
	header.magic = SPILL_MAGIC;
	header.nrows = writer->nrows;
	header.ncolumns = set->ncolumns;
	LogicalTapeWrite(set->tapes[partition], &header, sizeof(header));
	LogicalTapeWrite(set->tapes[partition], writer->hashes,
					 sizeof(uint32) * writer->nrows);
	LogicalTapeWrite(set->tapes[partition], writer->validity,
					 8 * set->ncolumns);
	for (int column = 0; column < set->ncolumns; column++)
		LogicalTapeWrite(set->tapes[partition],
			&writer->values[column * PG_BATCH_SPILL_BLOCK_ROWS],
			sizeof(int32) * writer->nrows);
	set->stats.blocks_written++;
	set->stats.bytes_written += block_bytes(set->ncolumns, writer->nrows);
	writer->nrows = 0;
	MemSet(writer->validity, 0, 8 * set->ncolumns);
}

static SpillWriter *
get_writer(PgBatchSpillSet *set, int partition)
{
	SpillWriter *writer = set->writers[partition];

	if (writer == NULL)
	{
		if (set->active_writers == set->max_writers)
		{
			for (int offset = 0; offset < set->npartitions; offset++)
			{
				int			victim = (set->eviction_cursor + offset) %
					set->npartitions;

				if (set->writers[victim] == NULL)
					continue;
				flush_writer(set, victim);
				free_writer(set, victim);
				set->eviction_cursor = (victim + 1) % set->npartitions;
				break;
			}
		}
		writer = MemoryContextAllocZero(set->context, sizeof(*writer));
		writer->values = MemoryContextAllocZero(set->context,
			sizeof(int32) * set->ncolumns * PG_BATCH_SPILL_BLOCK_ROWS);
		writer->validity = MemoryContextAllocZero(set->context,
			8 * set->ncolumns);
		set->writers[partition] = writer;
		set->active_writers++;
	}
	return writer;
}

PgBatchSpillSet *
pg_batch_spill_create(MemoryContext parent_context, TupleDesc tuple_desc,
					  int ncolumns, int npartitions, Size buffer_limit)
{
	MemoryContext oldcontext;
	PgBatchSpillSet *set;

	if (parent_context == NULL || tuple_desc == NULL || ncolumns <= 0 ||
		ncolumns > tuple_desc->natts || npartitions <= 0)
		elog(ERROR, "invalid pg_batch spill configuration");
	for (int column = 0; column < ncolumns; column++)
	{
		Form_pg_attribute attr = TupleDescAttr(tuple_desc, column);

		if (attr->attisdropped || attr->atttypid != INT4OID)
			elog(ERROR, "pg_batch spill column %d must be int4", column + 1);
	}
	oldcontext = MemoryContextSwitchTo(parent_context);
	set = palloc0_object(PgBatchSpillSet);
	set->context = AllocSetContextCreate(parent_context, "pg_batch spill",
										 ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(set->context);
	set->ncolumns = ncolumns;
	set->npartitions = npartitions;
	set->max_writers = (int) Min((Size) npartitions,
		Max((Size) 1, buffer_limit / writer_bytes(ncolumns)));
	set->tapeset = LogicalTapeSetCreate(true, NULL, -1);
	set->tapes = palloc_array(LogicalTape *, npartitions);
	set->writers = palloc0_array(SpillWriter *, npartitions);
	set->partition_rows = palloc0_array(uint64, npartitions);
	for (int partition = 0; partition < npartitions; partition++)
		set->tapes[partition] = LogicalTapeCreate(set->tapeset);
	MemoryContextSwitchTo(oldcontext);
	return set;
}

void
pg_batch_spill_write(PgBatchSpillSet *set, int partition,
					 const PgBatchInt4Vector *columns,
					 const PgBatchSelection *selection,
					 const uint32 *hashes)
{
	SpillWriter *writer;
	uint64		rows;

	validate_partition(set, partition);
	if (set->finished || columns == NULL || selection == NULL || hashes == NULL ||
		selection->nrows < 0 || selection->nrows > PG_BATCH_SPILL_BLOCK_ROWS ||
		selection->nwords != 1 || selection->words == NULL)
		elog(ERROR, "invalid pg_batch spill write");
	rows = selection->words[0] &
		(selection->nrows == 64 ? UINT64_MAX :
		 (UINT64CONST(1) << selection->nrows) - 1);
	if (rows == 0)
		return;
	writer = get_writer(set, partition);
	while (rows != 0)
	{
		int			row = pg_rightmost_one_pos64(rows);
		int			dest = writer->nrows++;

		writer->hashes[dest] = hashes[row];
		for (int column = 0; column < set->ncolumns; column++)
		{
			bool		isnull = pg_batch_int4_row_is_null(&columns[column], row);

			writer->values[column * PG_BATCH_SPILL_BLOCK_ROWS + dest] =
				isnull ? 0 : pg_batch_int4_row_value(&columns[column], row);
			if (!isnull)
				writer->validity[column * 8 + dest / 8] |=
					(uint8) 1 << (dest % 8);
		}
		set->partition_rows[partition]++;
		set->stats.rows_written++;
		if (writer->nrows == PG_BATCH_SPILL_BLOCK_ROWS)
			flush_writer(set, partition);
		rows &= rows - 1;
	}
}

void
pg_batch_spill_write_dense(PgBatchSpillSet *set, int partition,
					   int nrows, const uint32 *hashes,
					   const int32 *values, int value_stride,
					   const uint64 *validity)
{
	SpillWriter *writer;

	validate_partition(set, partition);
	if (set->finished || nrows <= 0 || nrows > PG_BATCH_SPILL_BLOCK_ROWS ||
		hashes == NULL || values == NULL || validity == NULL ||
		value_stride < nrows)
		elog(ERROR, "invalid dense pg_batch spill write");
	writer = get_writer(set, partition);
	for (int row = 0; row < nrows; row++)
	{
		int			dest = writer->nrows++;

		writer->hashes[dest] = hashes[row];
		for (int column = 0; column < set->ncolumns; column++)
		{
			writer->values[column * PG_BATCH_SPILL_BLOCK_ROWS + dest] =
				values[column * value_stride + row];
			if ((validity[column] & (UINT64CONST(1) << row)) != 0)
				writer->validity[column * 8 + dest / 8] |=
					(uint8) 1 << (dest % 8);
		}
		set->partition_rows[partition]++;
		set->stats.rows_written++;
		if (writer->nrows == PG_BATCH_SPILL_BLOCK_ROWS)
			flush_writer(set, partition);
	}
}

void
pg_batch_spill_finish(PgBatchSpillSet *set, bool frozen)
{
	if (set->finished)
		elog(ERROR, "pg_batch spill set is already finished");
	for (int partition = 0; partition < set->npartitions; partition++)
	{
		flush_writer(set, partition);
		if (set->partition_rows[partition] != 0)
		{
			if (frozen)
				LogicalTapeFreeze(set->tapes[partition], NULL);
			else
				LogicalTapeRewindForRead(set->tapes[partition], BLCKSZ);
		}
		free_writer(set, partition);
	}
	set->finished = true;
	set->frozen = frozen;
}

bool
pg_batch_spill_partition_empty(const PgBatchSpillSet *set, int partition)
{
	validate_partition(set, partition);
	return set->partition_rows[partition] == 0;
}

uint64
pg_batch_spill_partition_rows(const PgBatchSpillSet *set, int partition)
{
	validate_partition(set, partition);
	return set->partition_rows[partition];
}

PgBatchSpillReader *
pg_batch_spill_reader_open(PgBatchSpillSet *set, int partition)
{
	PgBatchSpillReader *reader;

	validate_partition(set, partition);
	if (!set->finished)
		elog(ERROR, "pg_batch spill set is not ready for reading");
	reader = MemoryContextAllocZero(set->context, sizeof(*reader));
	reader->set = set;
	reader->tape = set->tapes[partition];
	reader->partition = partition;
	reader->eof = pg_batch_spill_partition_empty(set, partition);
	reader->values = MemoryContextAlloc(set->context,
		sizeof(int32) * set->ncolumns * PG_BATCH_SPILL_BLOCK_ROWS);
	reader->validity = MemoryContextAlloc(set->context,
		8 * set->ncolumns);
	if (set->frozen && set->partition_rows[partition] != 0)
		LogicalTapeRewindForRead(reader->tape, BLCKSZ);
	return reader;
}

bool
pg_batch_spill_reader_next(PgBatchSpillReader *reader,
						   PgBatchSpillBlock *result)
{
	PgBatchSpillSet *set = reader->set;
	SpillHeader header;
	size_t		read;

	if (reader->eof)
		return false;
	read = LogicalTapeRead(reader->tape, &header, sizeof(header));
	if (read == 0)
	{
		reader->eof = true;
		return false;
	}
	if (read != sizeof(header) || header.magic != SPILL_MAGIC ||
		header.ncolumns != set->ncolumns || header.nrows == 0 ||
		header.nrows > PG_BATCH_SPILL_BLOCK_ROWS)
		elog(ERROR, "invalid pg_batch spill block");
	if (LogicalTapeRead(reader->tape, reader->hashes,
						sizeof(uint32) * header.nrows) !=
		sizeof(uint32) * header.nrows ||
		LogicalTapeRead(reader->tape, reader->validity,
						8 * set->ncolumns) != 8 * set->ncolumns)
		elog(ERROR, "unexpected end of pg_batch spill file");
	for (int column = 0; column < set->ncolumns; column++)
	{
		if (LogicalTapeRead(reader->tape,
				&reader->values[column * PG_BATCH_SPILL_BLOCK_ROWS],
				sizeof(int32) * header.nrows) !=
			sizeof(int32) * header.nrows)
			elog(ERROR, "unexpected end of pg_batch spill file");
	}
	result->nrows = header.nrows;
	result->ncolumns = set->ncolumns;
	result->selection = header.nrows == 64 ? UINT64_MAX :
		(UINT64CONST(1) << header.nrows) - 1;
	result->hashes = reader->hashes;
	result->values = reader->values;
	result->validity = reader->validity;
	set->stats.rows_read += header.nrows;
	set->stats.blocks_read++;
	set->stats.bytes_read += block_bytes(set->ncolumns, header.nrows);
	return true;
}

void
pg_batch_spill_reader_rewind(PgBatchSpillReader *reader)
{
	if (!reader->set->frozen)
		elog(ERROR, "cannot rewind a destructive pg_batch spill reader");
	reader->eof = pg_batch_spill_partition_empty(reader->set,
		reader->partition);
	if (!pg_batch_spill_partition_empty(reader->set, reader->partition))
		LogicalTapeRewindForRead(reader->tape, BLCKSZ);
}

void
pg_batch_spill_reader_close(PgBatchSpillReader *reader)
{
	if (reader == NULL)
		return;
	pfree(reader->values);
	pfree(reader->validity);
	pfree(reader);
}

const PgBatchSpillStats *
pg_batch_spill_stats(const PgBatchSpillSet *set)
{
	return &set->stats;
}

void
pg_batch_spill_destroy(PgBatchSpillSet *set)
{
	MemoryContext context;

	if (set == NULL)
		return;
	context = set->context;
	LogicalTapeSetClose(set->tapeset);
	MemoryContextDelete(context);
	pfree(set);
}

Size
pg_batch_shared_spill_estimate(int participants, int partitions)
{
	Size		result;

	if (participants <= 0 || partitions <= 0)
		elog(ERROR, "invalid shared pg_batch spill dimensions");
	result = MAXALIGN(sizeof(PgBatchSharedSpill));
	result = add_size(result,
		mul_size(sizeof(uint64), (Size) participants * partitions));
	result = add_size(result,
		mul_size(sizeof(PgBatchSpillStats), participants));
	result = add_size(result, participants);
	return MAXALIGN(result);
}

PgBatchSharedSpill *
pg_batch_shared_spill_initialize(void *shared_memory, dsm_segment *segment,
								 int participants, int partitions,
								 int ncolumns)
{
	PgBatchSharedSpill *shared = shared_memory;
	Size		size;

	if (shared_memory == NULL || segment == NULL || ncolumns <= 0)
		elog(ERROR, "invalid shared pg_batch spill initialization");
	size = pg_batch_shared_spill_estimate(participants, partitions);
	MemSet(shared, 0, size);
	shared->participants = participants;
	shared->partitions = partitions;
	shared->ncolumns = ncolumns;
	SharedFileSetInit(&shared->fileset, segment);
	return shared;
}

PgBatchSharedSpill *
pg_batch_shared_spill_attach(void *shared_memory, dsm_segment *segment)
{
	PgBatchSharedSpill *shared = shared_memory;

	if (shared == NULL || segment == NULL || shared->participants <= 0 ||
		shared->partitions <= 0 || shared->ncolumns <= 0)
		elog(ERROR, "invalid shared pg_batch spill attachment");
	SharedFileSetAttach(&shared->fileset, segment);
	return shared;
}

void
pg_batch_shared_spill_reset(PgBatchSharedSpill *shared)
{
	Size		rows_size;

	if (shared == NULL)
		return;
	SharedFileSetDeleteAll(&shared->fileset);
	rows_size = sizeof(uint64) * shared->participants * shared->partitions;
	MemSet(shared_rows(shared), 0, rows_size);
	MemSet(shared_stats(shared), 0,
		   sizeof(PgBatchSpillStats) * shared->participants);
	MemSet(shared_finished(shared), 0, shared->participants);
}

static void
validate_shared_partition(const PgBatchSharedSpill *shared, int partition)
{
	if (partition < 0 || partition >= shared->partitions)
		elog(ERROR, "pg_batch shared spill partition %d is out of range",
			 partition);
}

static void
shared_flush_writer(PgBatchSharedSpillWriter *writer, int partition)
{
	SpillWriter *block = writer->writers[partition];
	SpillHeader header;
	BufFile    *file;

	if (block == NULL || block->nrows == 0)
		return;
	file = writer->files[partition];
	if (file == NULL)
	{
		char		name[MAXPGPATH];

		shared_file_name(name, writer->participant, partition);
		file = BufFileCreateFileSet(&writer->shared->fileset.fs, name);
		writer->files[partition] = file;
	}
	header.magic = SPILL_MAGIC;
	header.nrows = block->nrows;
	header.ncolumns = writer->ncolumns;
	BufFileWrite(file, &header, sizeof(header));
	BufFileWrite(file, block->hashes, sizeof(uint32) * block->nrows);
	BufFileWrite(file, block->validity, 8 * writer->ncolumns);
	for (int column = 0; column < writer->ncolumns; column++)
		BufFileWrite(file,
			&block->values[column * PG_BATCH_SPILL_BLOCK_ROWS],
			sizeof(int32) * block->nrows);
	writer->stats.blocks_written++;
	writer->stats.bytes_written += block_bytes(writer->ncolumns, block->nrows);
	block->nrows = 0;
	MemSet(block->validity, 0, 8 * writer->ncolumns);
}

static void
shared_free_writer(PgBatchSharedSpillWriter *writer, int partition)
{
	SpillWriter *block = writer->writers[partition];

	if (block == NULL)
		return;
	pfree(block->values);
	pfree(block->validity);
	pfree(block);
	writer->writers[partition] = NULL;
	writer->active_writers--;
}

static SpillWriter *
shared_get_writer(PgBatchSharedSpillWriter *writer, int partition)
{
	SpillWriter *block = writer->writers[partition];

	if (block != NULL)
		return block;
	if (writer->active_writers == writer->max_writers)
	{
		for (int offset = 0; offset < writer->shared->partitions; offset++)
		{
			int			victim = (writer->eviction_cursor + offset) %
				writer->shared->partitions;

			if (writer->writers[victim] == NULL)
				continue;
			shared_flush_writer(writer, victim);
			shared_free_writer(writer, victim);
			writer->eviction_cursor = (victim + 1) %
				writer->shared->partitions;
			break;
		}
	}
	block = MemoryContextAllocZero(writer->context, sizeof(*block));
	block->values = MemoryContextAllocZero(writer->context,
		sizeof(int32) * writer->ncolumns * PG_BATCH_SPILL_BLOCK_ROWS);
	block->validity = MemoryContextAllocZero(writer->context,
		8 * writer->ncolumns);
	writer->writers[partition] = block;
	writer->active_writers++;
	return block;
}

PgBatchSharedSpillWriter *
pg_batch_shared_spill_writer_create(MemoryContext parent_context,
									PgBatchSharedSpill *shared,
									int participant, TupleDesc tuple_desc,
									Size buffer_limit)
{
	MemoryContext oldcontext;
	PgBatchSharedSpillWriter *writer;

	if (parent_context == NULL || shared == NULL || tuple_desc == NULL ||
		participant < 0 || participant >= shared->participants ||
		shared->ncolumns > tuple_desc->natts)
		elog(ERROR, "invalid shared pg_batch spill writer configuration");
	for (int column = 0; column < shared->ncolumns; column++)
	{
		Form_pg_attribute attr = TupleDescAttr(tuple_desc, column);

		if (attr->attisdropped || attr->atttypid != INT4OID)
			elog(ERROR, "pg_batch shared spill column %d must be int4",
				 column + 1);
	}
	oldcontext = MemoryContextSwitchTo(parent_context);
	writer = palloc0_object(PgBatchSharedSpillWriter);
	writer->context = AllocSetContextCreate(parent_context,
		"pg_batch shared spill writer", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(writer->context);
	writer->shared = shared;
	writer->participant = participant;
	writer->ncolumns = shared->ncolumns;
	writer->files = palloc0_array(BufFile *, shared->partitions);
	writer->writers = palloc0_array(SpillWriter *, shared->partitions);
	writer->partition_rows = palloc0_array(uint64, shared->partitions);
	writer->max_writers = (int) Min((Size) shared->partitions,
		Max((Size) 1, buffer_limit / writer_bytes(shared->ncolumns)));
	MemoryContextSwitchTo(oldcontext);
	return writer;
}

void
pg_batch_shared_spill_write(PgBatchSharedSpillWriter *writer, int partition,
							const PgBatchInt4Vector *columns,
							const PgBatchSelection *selection,
							const uint32 *hashes)
{
	SpillWriter *block;
	uint64		rows;

	validate_shared_partition(writer->shared, partition);
	if (writer->finished || columns == NULL || selection == NULL ||
		hashes == NULL || selection->nrows < 0 ||
		selection->nrows > PG_BATCH_SPILL_BLOCK_ROWS ||
		selection->nwords != 1 || selection->words == NULL)
		elog(ERROR, "invalid shared pg_batch spill write");
	rows = selection->words[0] &
		(selection->nrows == 64 ? UINT64_MAX :
		 (UINT64CONST(1) << selection->nrows) - 1);
	if (rows == 0)
		return;
	block = shared_get_writer(writer, partition);
	while (rows != 0)
	{
		int			row = pg_rightmost_one_pos64(rows);
		int			dest = block->nrows++;

		block->hashes[dest] = hashes[row];
		for (int column = 0; column < writer->ncolumns; column++)
		{
			bool		isnull = pg_batch_int4_row_is_null(&columns[column], row);

			block->values[column * PG_BATCH_SPILL_BLOCK_ROWS + dest] =
				isnull ? 0 : pg_batch_int4_row_value(&columns[column], row);
			if (!isnull)
				block->validity[column * 8 + dest / 8] |=
					(uint8) 1 << (dest % 8);
		}
		writer->partition_rows[partition]++;
		writer->stats.rows_written++;
		if (block->nrows == PG_BATCH_SPILL_BLOCK_ROWS)
			shared_flush_writer(writer, partition);
		rows &= rows - 1;
	}
}

void
pg_batch_shared_spill_write_dense(PgBatchSharedSpillWriter *writer,
								   int partition, int nrows,
								   const uint32 *hashes,
								   const int32 *values, int value_stride,
								   const uint64 *validity)
{
	SpillWriter *block;

	validate_shared_partition(writer->shared, partition);
	if (writer->finished || nrows <= 0 ||
		nrows > PG_BATCH_SPILL_BLOCK_ROWS || hashes == NULL || values == NULL ||
		validity == NULL || value_stride < nrows)
		elog(ERROR, "invalid dense shared pg_batch spill write");
	block = shared_get_writer(writer, partition);
	for (int row = 0; row < nrows; row++)
	{
		int			dest = block->nrows++;

		block->hashes[dest] = hashes[row];
		for (int column = 0; column < writer->ncolumns; column++)
		{
			block->values[column * PG_BATCH_SPILL_BLOCK_ROWS + dest] =
				values[column * value_stride + row];
			if ((validity[column] & (UINT64CONST(1) << row)) != 0)
				block->validity[column * 8 + dest / 8] |=
					(uint8) 1 << (dest % 8);
		}
		writer->partition_rows[partition]++;
		writer->stats.rows_written++;
		if (block->nrows == PG_BATCH_SPILL_BLOCK_ROWS)
			shared_flush_writer(writer, partition);
	}
}

void
pg_batch_shared_spill_writer_finish(PgBatchSharedSpillWriter *writer)
{
	uint64	   *rows;

	if (writer->finished)
		elog(ERROR, "pg_batch shared spill writer is already finished");
	for (int partition = 0; partition < writer->shared->partitions; partition++)
	{
		shared_flush_writer(writer, partition);
		shared_free_writer(writer, partition);
		if (writer->files[partition] != NULL)
		{
			BufFileExportFileSet(writer->files[partition]);
			BufFileClose(writer->files[partition]);
			writer->files[partition] = NULL;
		}
	}
	rows = shared_rows(writer->shared) +
		writer->participant * writer->shared->partitions;
	memcpy(rows, writer->partition_rows,
		   sizeof(uint64) * writer->shared->partitions);
	shared_stats(writer->shared)[writer->participant] = writer->stats;
	pg_write_barrier();
	shared_finished(writer->shared)[writer->participant] = 1;
	writer->finished = true;
}

void
pg_batch_shared_spill_writer_destroy(PgBatchSharedSpillWriter *writer)
{
	MemoryContext context;

	if (writer == NULL)
		return;
	for (int partition = 0; partition < writer->shared->partitions; partition++)
	{
		if (writer->files[partition] != NULL)
			BufFileClose(writer->files[partition]);
	}
	context = writer->context;
	MemoryContextDelete(context);
	pfree(writer);
}

uint64
pg_batch_shared_spill_partition_rows(const PgBatchSharedSpill *shared,
									 int partition)
{
	uint64		result = 0;

	validate_shared_partition(shared, partition);
	for (int participant = 0; participant < shared->participants; participant++)
		result += shared_rows(shared)[participant * shared->partitions +
			partition];
	return result;
}

static void
shared_reader_close_file(PgBatchSharedSpillReader *reader)
{
	if (reader->file != NULL)
	{
		BufFileClose(reader->file);
		reader->file = NULL;
	}
}

static bool
shared_reader_open_next(PgBatchSharedSpillReader *reader)
{
	PgBatchSharedSpill *shared = reader->shared;

	shared_reader_close_file(reader);
	while (++reader->participant < shared->participants)
	{
		uint64		rows = shared_rows(shared)[
			reader->participant * shared->partitions + reader->partition];

		if (rows == 0)
			continue;
		if (!shared_finished(shared)[reader->participant])
			elog(ERROR, "pg_batch shared spill participant is still writing");
		{
			char		name[MAXPGPATH];

			shared_file_name(name, reader->participant, reader->partition);
			reader->file = BufFileOpenFileSet(&shared->fileset.fs, name,
				O_RDONLY, false);
		}
		return true;
	}
	return false;
}

PgBatchSharedSpillReader *
pg_batch_shared_spill_reader_open(MemoryContext parent_context,
								  PgBatchSharedSpill *shared, int partition)
{
	MemoryContext oldcontext;
	PgBatchSharedSpillReader *reader;

	if (parent_context == NULL || shared == NULL)
		elog(ERROR, "invalid shared pg_batch spill reader configuration");
	validate_shared_partition(shared, partition);
	oldcontext = MemoryContextSwitchTo(parent_context);
	reader = palloc0_object(PgBatchSharedSpillReader);
	reader->context = AllocSetContextCreate(parent_context,
		"pg_batch shared spill reader", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(reader->context);
	reader->shared = shared;
	reader->partition = partition;
	reader->participant = -1;
	reader->values = palloc(sizeof(int32) * shared->ncolumns *
		PG_BATCH_SPILL_BLOCK_ROWS);
	reader->validity = palloc(8 * shared->ncolumns);
	MemoryContextSwitchTo(oldcontext);
	return reader;
}

bool
pg_batch_shared_spill_reader_next(PgBatchSharedSpillReader *reader,
								  PgBatchSpillBlock *result)
{
	SpillHeader header;
	size_t		read;

	for (;;)
	{
		if (reader->file == NULL && !shared_reader_open_next(reader))
			return false;
		read = BufFileRead(reader->file, &header, sizeof(header));
		if (read == 0)
		{
			shared_reader_close_file(reader);
			continue;
		}
		if (read != sizeof(header) || header.magic != SPILL_MAGIC ||
			header.ncolumns != reader->shared->ncolumns ||
			header.nrows == 0 || header.nrows > PG_BATCH_SPILL_BLOCK_ROWS)
			elog(ERROR, "invalid shared pg_batch spill block");
		BufFileReadExact(reader->file, reader->hashes,
						 sizeof(uint32) * header.nrows);
		BufFileReadExact(reader->file, reader->validity,
						 8 * reader->shared->ncolumns);
		for (int column = 0; column < reader->shared->ncolumns; column++)
			BufFileReadExact(reader->file,
				&reader->values[column * PG_BATCH_SPILL_BLOCK_ROWS],
				sizeof(int32) * header.nrows);
		result->nrows = header.nrows;
		result->ncolumns = reader->shared->ncolumns;
		result->selection = header.nrows == 64 ? UINT64_MAX :
			(UINT64CONST(1) << header.nrows) - 1;
		result->hashes = reader->hashes;
		result->values = reader->values;
		result->validity = reader->validity;
		reader->stats.rows_read += header.nrows;
		reader->stats.blocks_read++;
		reader->stats.bytes_read += block_bytes(reader->shared->ncolumns,
										 header.nrows);
		return true;
	}
}

void
pg_batch_shared_spill_reader_rewind(PgBatchSharedSpillReader *reader)
{
	shared_reader_close_file(reader);
	reader->participant = -1;
}

void
pg_batch_shared_spill_reader_close(PgBatchSharedSpillReader *reader)
{
	MemoryContext context;

	if (reader == NULL)
		return;
	shared_reader_close_file(reader);
	context = reader->context;
	MemoryContextDelete(context);
	pfree(reader);
}

const PgBatchSpillStats *
pg_batch_shared_spill_reader_stats(const PgBatchSharedSpillReader *reader)
{
	return &reader->stats;
}

void
pg_batch_shared_spill_stats(const PgBatchSharedSpill *shared,
							PgBatchSpillStats *result)
{
	MemSet(result, 0, sizeof(*result));
	for (int participant = 0; participant < shared->participants; participant++)
	{
		const PgBatchSpillStats *stats = &shared_stats(shared)[participant];

		result->rows_written += stats->rows_written;
		result->blocks_written += stats->blocks_written;
		result->bytes_written += stats->bytes_written;
	}
}
