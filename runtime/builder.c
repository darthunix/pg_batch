#include "postgres.h"

#include "executor/tuptable.h"
#include "utils/datum.h"
#include "utils/memutils.h"

#include "runtime.h"

struct PgBatchBuilder
{
	PgBatch batch;
	TupleDesc	tuple_desc;
	MemoryContext payload_context;
	Datum	   *values;
	bool	   *isnull;
	uint64	   *selection;
	int			ncolumns;
	int			capacity;
	int			capacity_nwords;
	int			nrows;
	bool		has_byref;
	bool		sealed;
};

static void
builder_get_column(PgBatch *batch, int column,
						const PgBatchSelection *rows,
						PgBatchColumnPhase phase,
						PgBatchDatumVector *result)
{
	PgBatchBuilder *builder = batch->private_data;

	pg_batch_check_datum_vector(result);
	if (column < 0 || column >= builder->ncolumns)
		elog(ERROR, "pg_batch builder column is out of range");
	result->values = &builder->values[column * builder->capacity];
	result->isnull = &builder->isnull[column * builder->capacity];
	result->nrows = batch->selection.nrows;
}

static const PgBatchOps builder_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_OPS_ABI_VERSION, PgBatchOps),
	.get_datum_column = builder_get_column,
};

static inline int
words_for_rows(int nrows)
{
	return nrows / 64 + (nrows % 64 != 0);
}

PgBatchBuilder *
pg_batch_builder_create(const PgBatchBuilderConfig *config)
{
	PgBatchBuilder *builder;
	Size		nvalues;

	if (config == NULL ||
		config->struct_size < PG_BATCH_BUILDER_CONFIG_MIN_SIZE ||
		config->parent_context == NULL || config->tuple_desc == NULL)
		elog(ERROR, "pg_batch builder requires a memory context and tuple descriptor");
	if (config->ncolumns <= 0 || config->ncolumns > config->tuple_desc->natts)
		elog(ERROR, "invalid pg_batch builder column count");
	if (config->capacity <= 0)
		elog(ERROR, "invalid pg_batch builder capacity");

	nvalues = mul_size(config->ncolumns, config->capacity);
	builder = MemoryContextAllocZero(config->parent_context, sizeof(*builder));
	builder->tuple_desc = config->tuple_desc;
	builder->ncolumns = config->ncolumns;
	builder->capacity = config->capacity;
	builder->capacity_nwords = words_for_rows(config->capacity);
	builder->values = MemoryContextAlloc(config->parent_context,
									  mul_size(sizeof(Datum), nvalues));
	builder->isnull = MemoryContextAlloc(config->parent_context,
									  mul_size(sizeof(bool), nvalues));
	builder->selection = MemoryContextAlloc(config->parent_context,
		mul_size(sizeof(uint64), builder->capacity_nwords));
	builder->payload_context =
		AllocSetContextCreate(config->parent_context, "pg_batch builder values",
							  ALLOCSET_DEFAULT_SIZES);
	for (int column = 0; column < config->ncolumns; column++)
	{
		if (!TupleDescAttr(config->tuple_desc, column)->attbyval)
		{
			builder->has_byref = true;
			break;
		}
	}
	builder->batch.abi_version = PG_BATCH_ABI_VERSION;
	builder->batch.struct_size = sizeof(PgBatch);
	builder->batch.ops = &builder_ops;
	builder->batch.private_data = builder;
	pg_batch_builder_reset(builder);
	return builder;
}

void
pg_batch_builder_reset(PgBatchBuilder *builder)
{
	MemoryContextReset(builder->payload_context);
	builder->nrows = 0;
	builder->sealed = false;
	builder->batch.selection.nrows = 0;
	builder->batch.selection.nwords = 0;
	builder->batch.table_oid = InvalidOid;
}

bool
pg_batch_builder_is_full(const PgBatchBuilder *builder)
{
	return builder->sealed || builder->nrows == builder->capacity;
}

void
pg_batch_builder_append_slot(PgBatchBuilder *builder,
								  TupleTableSlot *slot)
{
	MemoryContext oldcontext = NULL;
	int			row = builder->nrows;

	if (unlikely(builder->sealed))
		elog(ERROR, "cannot append to a sealed pg_batch builder");
	if (unlikely(row >= builder->capacity))
		elog(ERROR, "pg_batch builder is full");
	if (unlikely(slot->tts_tupleDescriptor->natts < builder->ncolumns))
		elog(ERROR, "pg_batch builder input has too few columns");

	slot_getsomeattrs(slot, builder->ncolumns);
	if (likely(builder->ncolumns == 1 && !builder->has_byref))
	{
		builder->isnull[row] = slot->tts_isnull[0];
		builder->values[row] = slot->tts_isnull[0] ?
			(Datum) 0 : slot->tts_values[0];
		builder->nrows = row + 1;
		return;
	}
	if (!builder->has_byref)
	{
		for (int column = 0; column < builder->ncolumns; column++)
		{
			int			offset = column * builder->capacity + row;

			builder->isnull[offset] = slot->tts_isnull[column];
			builder->values[offset] = slot->tts_isnull[column] ?
				(Datum) 0 : slot->tts_values[column];
		}
		builder->nrows = row + 1;
		return;
	}
	oldcontext = MemoryContextSwitchTo(builder->payload_context);
	for (int column = 0; column < builder->ncolumns; column++)
	{
		Form_pg_attribute attr = TupleDescAttr(builder->tuple_desc, column);
		int			offset = column * builder->capacity + row;

		builder->isnull[offset] = slot->tts_isnull[column];
		if (slot->tts_isnull[column])
			builder->values[offset] = (Datum) 0;
		else if (attr->attbyval)
			builder->values[offset] = slot->tts_values[column];
		else
			builder->values[offset] =
				datumCopy(slot->tts_values[column], false, attr->attlen);
	}
	MemoryContextSwitchTo(oldcontext);
	builder->nrows++;
}

PgBatch *
pg_batch_builder_finish(PgBatchBuilder *builder, Oid table_oid)
{
	if (builder->sealed)
		return builder->nrows == 0 ? NULL : &builder->batch;
	builder->sealed = true;
	if (builder->nrows == 0)
		return NULL;

	builder->batch.selection.nrows = builder->nrows;
	builder->batch.selection.nwords = words_for_rows(builder->nrows);
	builder->batch.table_oid = table_oid;
	for (int word = 0; word < builder->batch.selection.nwords; word++)
	{
		int			remaining = builder->nrows - word * 64;

		builder->selection[word] = remaining >= 64 ? UINT64_MAX :
			UINT64_MAX >> (64 - remaining);
	}
	builder->batch.selection.words = builder->selection;
	return &builder->batch;
}
