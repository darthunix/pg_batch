#include "postgres.h"

#include "fmgr.h"

#include "kernels.h"
#include "runtime.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_batch_kernels_test);

typedef struct TestDatumSource
{
	const Datum *values;
	const bool *isnull;
	const uint64 *available;
} TestDatumSource;

static void
test_get_datum_column(PgBatchBridgeBatch *batch, int column,
					  const uint64 *rows,
					  PgBatchBridgeMaterializePhase phase,
					  PgBatchBridgeDatumColumn *result)
{
	TestDatumSource *source = batch->private_data;

	Assert(column == 0);
	Assert(rows != NULL);
	Assert(phase == PG_BATCH_BRIDGE_MATERIALIZE_FILTER);
	result->values = source->values;
	result->isnull = source->isnull;
	result->valid_rows = source->available;
	result->nwords = 1;
}

static const PgBatchBridgeBatchOps test_batch_ops = {
	.abi_version = PG_BATCH_BRIDGE_ABI_VERSION,
	.struct_size = sizeof(PgBatchBridgeBatchOps),
	.get_datum_column = test_get_datum_column,
};

Datum
pg_batch_kernels_test(PG_FUNCTION_ARGS)
{
	Datum		datum_values[6];
	bool		datum_nulls[6] = {false, false, true, false, false, false};
	uint64		available = UINT64CONST(0x3f);
	int32		packed_values[7] = {99, 1, 2, 3, 4, 5, 6};
	uint8		packed_validity[1] = {0x76};
	PgBatchInt4Vector datum;
	PgBatchInt4Vector packed;
	TestDatumSource source;
	PgBatchBridgeBatch batch;
	PgBatchInt4Reduction datum_reduction;
	PgBatchInt4Reduction packed_reduction;
	uint64		datum_selection = UINT64CONST(0x3f);
	uint64		packed_selection = UINT64CONST(0x3f);
	uint64		datum_hash_rows;
	uint64		packed_hash_rows;
	uint32		datum_hashes[6];
	uint32		packed_hashes[6];
	const PgBatchInt4Vector *datum_keys[1] = {&datum};
	const PgBatchInt4Vector *packed_keys[1] = {&packed};
	uint32		flags = PG_BATCH_INT4_REDUCE_COUNT |
		PG_BATCH_INT4_REDUCE_SUM | PG_BATCH_INT4_REDUCE_MIN |
		PG_BATCH_INT4_REDUCE_MAX;

	for (int row = 0; row < 6; row++)
		datum_values[row] = Int32GetDatum(row + 1);
	source.values = datum_values;
	source.isnull = datum_nulls;
	source.available = &available;
	MemSet(&batch, 0, sizeof(batch));
	batch.ops = &test_batch_ops;
	batch.private_data = &source;
	pg_batch_get_int4_vector(&batch, 0, &datum_selection,
						 PG_BATCH_BRIDGE_MATERIALIZE_FILTER, &datum);
	pg_batch_int4_vector_init_packed(&packed, packed_values,
								  packed_validity, 1);

	pg_batch_reduce_int4(&datum, 6, 1, &datum_selection, flags,
						 &datum_reduction);
	pg_batch_reduce_int4(&packed, 6, 1, &packed_selection, flags,
						 &packed_reduction);
	if (datum_reduction.count != 5 || datum_reduction.sum != 18 ||
		datum_reduction.min != 1 || datum_reduction.max != 6 ||
		memcmp(&datum_reduction, &packed_reduction,
			   sizeof(datum_reduction)) != 0)
		PG_RETURN_BOOL(false);

	pg_batch_hash_int4(datum_keys, 1, 6, 1, &datum_selection,
					   datum_hashes, &datum_hash_rows);
	pg_batch_hash_int4(packed_keys, 1, 6, 1, &packed_selection,
					   packed_hashes, &packed_hash_rows);
	if (datum_hash_rows != UINT64CONST(0x3b) ||
		packed_hash_rows != datum_hash_rows)
		PG_RETURN_BOOL(false);
	for (int row = 0; row < 6; row++)
	{
		if ((datum_hash_rows & (UINT64CONST(1) << row)) != 0 &&
			datum_hashes[row] != packed_hashes[row])
			PG_RETURN_BOOL(false);
	}

	pg_batch_filter_int4(&datum, 6, 1, &datum_selection,
						 PG_BATCH_INT4_GT, 3, true);
	pg_batch_filter_int4(&packed, 6, 1, &packed_selection,
						 PG_BATCH_INT4_GT, 3, true);
	if (datum_selection != UINT64CONST(0x38) ||
		packed_selection != datum_selection)
		PG_RETURN_BOOL(false);

	/* A Datum vector may leave rows unavailable when they are not selected. */
	available = UINT64CONST(0x21);
	datum_selection = available;
	pg_batch_filter_int4(&datum, 6, 1, &datum_selection,
						 PG_BATCH_INT4_GT, 3, false);
	PG_RETURN_BOOL(datum_selection == UINT64CONST(0x20));
}
