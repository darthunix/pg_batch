#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "utils/memutils.h"

#include "spill.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_batch_spill_test);

Datum
pg_batch_spill_test(PG_FUNCTION_ARGS)
{
	MemoryContext context;
	MemoryContext oldcontext;
	TupleDesc	desc;
	PgBatchSpillSet *set;
	PgBatchSpillReader *reader;
	PgBatchSpillBlock block;
	PgBatchSpillStats stats;
	Datum		first_values[8];
	Datum		second_values[8];
	bool		first_nulls[8] = {false};
	bool		second_nulls[8] = {false};
	PgBatchInt4Vector columns[2];
	uint32		hashes[8];
	uint64		selection;
	int32		dense_values[4] = {40, 41, 140, 141};
	uint64		dense_validity[2] = {UINT64CONST(0x3), UINT64CONST(0x1)};
	uint32		dense_hashes[2] = {400, 401};
	bool		ok = true;

	context = AllocSetContextCreate(CurrentMemoryContext,
		"pg_batch spill test", ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(context);
	desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(desc, 1, "first", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 2, "second", INT4OID, -1, 0);
	TupleDescFinalize(desc);
	for (int row = 0; row < 8; row++)
	{
		first_values[row] = Int32GetDatum(row + 10);
		second_values[row] = Int32GetDatum(row + 110);
		hashes[row] = row + 100;
	}
	second_nulls[2] = true;
	pg_batch_int4_vector_init_datum(&columns[0], first_values, first_nulls);
	pg_batch_int4_vector_init_datum(&columns[1], second_values, second_nulls);

	/* One writer block forces eviction as writes alternate partitions. */
	set = pg_batch_spill_create(context, desc, 2, 2, 1);
	selection = UINT64CONST(0x0d);
	pg_batch_spill_write(set, 0, columns,
		&(PgBatchSelection) {8, 1, &selection}, hashes);
	pg_batch_spill_write_dense(set, 1, 2, dense_hashes, dense_values, 2,
		dense_validity);
	selection = UINT64CONST(0x22);
	pg_batch_spill_write(set, 0, columns,
		&(PgBatchSelection) {8, 1, &selection}, hashes);
	pg_batch_spill_finish(set, true);
	if (pg_batch_spill_partition_rows(set, 0) != 5 ||
		pg_batch_spill_partition_rows(set, 1) != 2)
		ok = false;

	reader = pg_batch_spill_reader_open(set, 0);
	if (!pg_batch_spill_reader_next(reader, &block) || block.nrows != 3 ||
		block.ncolumns != 2 || block.selection != UINT64CONST(0x7) ||
		block.hashes[0] != 100 || block.hashes[1] != 102 ||
		block.hashes[2] != 103 || block.values[0] != 10 ||
		block.values[2] != 13 || block.values[64] != 110 ||
		block.values[65] != 0 || block.validity[0] != UINT8_C(0x7) ||
		block.validity[8] != UINT8_C(0x5))
		ok = false;
	if (!pg_batch_spill_reader_next(reader, &block) || block.nrows != 2 ||
		block.hashes[0] != 101 || block.hashes[1] != 105 ||
		block.values[0] != 11 || block.values[1] != 15 ||
		block.validity[0] != UINT8_C(0x3) ||
		block.validity[8] != UINT8_C(0x3) ||
		pg_batch_spill_reader_next(reader, &block))
		ok = false;
	pg_batch_spill_reader_rewind(reader);
	if (!pg_batch_spill_reader_next(reader, &block) || block.nrows != 3)
		ok = false;
	pg_batch_spill_reader_close(reader);

	reader = pg_batch_spill_reader_open(set, 1);
	if (!pg_batch_spill_reader_next(reader, &block) || block.nrows != 2 ||
		block.hashes[0] != 400 || block.values[0] != 40 ||
		block.values[64] != 140 || block.validity[0] != UINT8_C(0x3) ||
		block.validity[8] != UINT8_C(0x1))
		ok = false;
	stats = *pg_batch_spill_stats(set);
	if (stats.rows_written != 7 || stats.blocks_written != 3 ||
		stats.rows_read != 10 || stats.blocks_read != 4)
		ok = false;
	pg_batch_spill_reader_close(reader);
	pg_batch_spill_destroy(set);
	FreeTupleDesc(desc);
	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(context);
	PG_RETURN_BOOL(ok);
}
