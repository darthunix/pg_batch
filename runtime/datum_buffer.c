#include "postgres.h"

#include "executor/tuptable.h"
#include "utils/datum.h"
#include "utils/memutils.h"

#include "runtime.h"

struct PgBatchDatumBuffer
{
	PgBatchBridgeBatch batch;
	TupleDesc	tuple_desc;
	MemoryContext payload_context;
	Datum	   *values;
	bool	   *isnull;
	uint64	   *valid_rows;
	uint64	   *selection;
	int			ncolumns;
	int			capacity;
	int			capacity_nwords;
	int			nrows;
	bool		has_byref;
	bool		sealed;
};

static void
datum_buffer_get_column(PgBatchBridgeBatch *batch, int column,
						const uint64 *rows,
						PgBatchBridgeMaterializePhase phase,
						PgBatchBridgeDatumColumn *result)
{
	PgBatchDatumBuffer *buffer = batch->private_data;
	const uint64 *valid_rows;

	if (column < 0 || column >= buffer->ncolumns)
		elog(ERROR, "pg_batch Datum buffer column is out of range");
	valid_rows = &buffer->valid_rows[column * buffer->capacity_nwords];
	for (int word = 0; word < batch->nwords; word++)
	{
		if ((rows[word] & ~valid_rows[word]) != 0)
			elog(ERROR, "pg_batch Datum buffer row is not materialized");
	}
	result->values = &buffer->values[column * buffer->capacity];
	result->isnull = &buffer->isnull[column * buffer->capacity];
	result->valid_rows = valid_rows;
	result->nwords = batch->nwords;
}

static const PgBatchBridgeBatchOps datum_buffer_ops = {
	.abi_version = PG_BATCH_BRIDGE_ABI_VERSION,
	.struct_size = sizeof(PgBatchBridgeBatchOps),
	.get_datum_column = datum_buffer_get_column,
};

static inline int
words_for_rows(int nrows)
{
	return nrows / 64 + (nrows % 64 != 0);
}

PgBatchDatumBuffer *
pg_batch_datum_buffer_create(MemoryContext parent_context,
							 TupleDesc tuple_desc, int ncolumns, int capacity)
{
	PgBatchDatumBuffer *buffer;
	Size		nvalues;

	if (parent_context == NULL || tuple_desc == NULL)
		elog(ERROR, "pg_batch Datum buffer requires a memory context and tuple descriptor");
	if (ncolumns <= 0 || ncolumns > tuple_desc->natts)
		elog(ERROR, "invalid pg_batch Datum buffer column count");
	if (capacity <= 0)
		elog(ERROR, "invalid pg_batch Datum buffer capacity");

	nvalues = mul_size(ncolumns, capacity);
	buffer = MemoryContextAllocZero(parent_context, sizeof(*buffer));
	buffer->tuple_desc = tuple_desc;
	buffer->ncolumns = ncolumns;
	buffer->capacity = capacity;
	buffer->capacity_nwords = words_for_rows(capacity);
	buffer->values = MemoryContextAlloc(parent_context,
									  mul_size(sizeof(Datum), nvalues));
	buffer->isnull = MemoryContextAlloc(parent_context,
									  mul_size(sizeof(bool), nvalues));
	buffer->valid_rows = MemoryContextAlloc(parent_context,
		mul_size(sizeof(uint64),
				 mul_size(ncolumns, buffer->capacity_nwords)));
	buffer->selection = MemoryContextAlloc(parent_context,
		mul_size(sizeof(uint64), buffer->capacity_nwords));
	buffer->payload_context =
		AllocSetContextCreate(parent_context, "pg_batch Datum buffer values",
							  ALLOCSET_DEFAULT_SIZES);
	for (int column = 0; column < ncolumns; column++)
	{
		if (!TupleDescAttr(tuple_desc, column)->attbyval)
		{
			buffer->has_byref = true;
			break;
		}
	}
	buffer->batch.abi_version = PG_BATCH_BRIDGE_ABI_VERSION;
	buffer->batch.struct_size = sizeof(PgBatchBridgeBatch);
	buffer->batch.ops = &datum_buffer_ops;
	buffer->batch.private_data = buffer;
	pg_batch_datum_buffer_reset(buffer);
	return buffer;
}

void
pg_batch_datum_buffer_reset(PgBatchDatumBuffer *buffer)
{
	MemoryContextReset(buffer->payload_context);
	buffer->nrows = 0;
	buffer->sealed = false;
	buffer->batch.nrows = 0;
	buffer->batch.nwords = 0;
	buffer->batch.table_oid = InvalidOid;
	buffer->batch.consumed = true;
}

bool
pg_batch_datum_buffer_is_full(const PgBatchDatumBuffer *buffer)
{
	return buffer->sealed || buffer->nrows == buffer->capacity;
}

void
pg_batch_datum_buffer_append_slot(PgBatchDatumBuffer *buffer,
								  TupleTableSlot *slot)
{
	MemoryContext oldcontext = NULL;
	int			row = buffer->nrows;

	if (unlikely(buffer->sealed))
		elog(ERROR, "cannot append to a sealed pg_batch Datum buffer");
	if (unlikely(row >= buffer->capacity))
		elog(ERROR, "pg_batch Datum buffer is full");
	if (unlikely(slot->tts_tupleDescriptor->natts < buffer->ncolumns))
		elog(ERROR, "pg_batch Datum buffer input has too few columns");

	slot_getsomeattrs(slot, buffer->ncolumns);
	if (likely(buffer->ncolumns == 1 && !buffer->has_byref))
	{
		buffer->isnull[row] = slot->tts_isnull[0];
		buffer->values[row] = slot->tts_isnull[0] ?
			(Datum) 0 : slot->tts_values[0];
		buffer->nrows = row + 1;
		return;
	}
	if (!buffer->has_byref)
	{
		for (int column = 0; column < buffer->ncolumns; column++)
		{
			int			offset = column * buffer->capacity + row;

			buffer->isnull[offset] = slot->tts_isnull[column];
			buffer->values[offset] = slot->tts_isnull[column] ?
				(Datum) 0 : slot->tts_values[column];
		}
		buffer->nrows = row + 1;
		return;
	}
	oldcontext = MemoryContextSwitchTo(buffer->payload_context);
	for (int column = 0; column < buffer->ncolumns; column++)
	{
		Form_pg_attribute attr = TupleDescAttr(buffer->tuple_desc, column);
		int			offset = column * buffer->capacity + row;

		buffer->isnull[offset] = slot->tts_isnull[column];
		if (slot->tts_isnull[column])
			buffer->values[offset] = (Datum) 0;
		else if (attr->attbyval)
			buffer->values[offset] = slot->tts_values[column];
		else
			buffer->values[offset] =
				datumCopy(slot->tts_values[column], false, attr->attlen);
	}
	MemoryContextSwitchTo(oldcontext);
	buffer->nrows++;
}

PgBatchBridgeBatch *
pg_batch_datum_buffer_finish(PgBatchDatumBuffer *buffer, Oid table_oid)
{
	if (buffer->sealed)
		return buffer->nrows == 0 ? NULL : &buffer->batch;
	buffer->sealed = true;
	if (buffer->nrows == 0)
		return NULL;

	buffer->batch.nrows = buffer->nrows;
	buffer->batch.nwords = words_for_rows(buffer->nrows);
	buffer->batch.table_oid = table_oid;
	for (int word = 0; word < buffer->batch.nwords; word++)
	{
		int			remaining = buffer->nrows - word * 64;

		buffer->selection[word] = remaining >= 64 ? UINT64_MAX :
			UINT64_MAX >> (64 - remaining);
	}
	for (int column = 0; column < buffer->ncolumns; column++)
		memcpy(&buffer->valid_rows[column * buffer->capacity_nwords],
			   buffer->selection,
			   sizeof(uint64) * buffer->batch.nwords);
	buffer->batch.selection = buffer->selection;
	return &buffer->batch;
}
