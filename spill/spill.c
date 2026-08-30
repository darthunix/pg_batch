#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "port/pg_bitutils.h"
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
					 const PgBatchInt4Vector *columns, int nrows,
					 const uint64 *selection, const uint32 *hashes)
{
	SpillWriter *writer;
	uint64		rows;

	validate_partition(set, partition);
	if (set->finished || columns == NULL || selection == NULL || hashes == NULL ||
		nrows < 0 || nrows > PG_BATCH_SPILL_BLOCK_ROWS)
		elog(ERROR, "invalid pg_batch spill write");
	rows = selection[0] & (nrows == 64 ? UINT64_MAX :
		(UINT64CONST(1) << nrows) - 1);
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
