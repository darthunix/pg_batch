#include "postgres.h"

#include "executor/executor.h"

#include "runtime.h"

struct PgBatchOutput
{
	const PgBatchAPI *api;
	TupleTableSlot *slot;
	PgBatchBinding *binding;
};

static void virtual_select_row(TupleTableSlot *slot, PgBatch *batch, int row);

static void
virtual_accept_batch(TupleTableSlot *slot, PgBatch *batch)
{
	int			row = pg_batch_next_selected(batch, -1);

	if (row < 0)
		elog(ERROR, "pg_batch source published an empty selection");
	virtual_select_row(slot, batch, row);
}

static void
virtual_select_row(TupleTableSlot *slot, PgBatch *batch, int row)
{
	TupleDesc	desc = slot->tts_tupleDescriptor;
	uint64		local_rows;
	uint64	   *rows;
	int			word = row / 64;
	uint64		bit = UINT64CONST(1) << (row % 64);

	Assert(slot->tts_ops == &TTSOpsVirtual);
	Assert(pg_batch_row_selected(batch, row));
	ExecClearTuple(slot);
	if (batch->nwords == 1)
	{
		local_rows = bit;
		rows = &local_rows;
	}
	else
	{
		rows = palloc0_array(uint64, batch->nwords);
		rows[word] = bit;
	}
	for (int column = 0; column < desc->natts; column++)
	{
		PgBatchDatumVector vector;

		pg_batch_get_datum_column(batch, column, rows,
								  PG_BATCH_COLUMN_PROJECT, &vector);
		if (word >= vector.nwords || (vector.valid_rows[word] & bit) == 0)
			elog(ERROR, "pg_batch source did not materialize column %d row %d",
				 column + 1, row);
		slot->tts_values[column] = vector.values[row];
		slot->tts_isnull[column] = vector.isnull[row];
	}
	if (batch->nwords > 1)
		pfree(rows);
	slot->tts_tableOid = batch->table_oid;
	ExecStoreVirtualTuple(slot);
}

static const PgBatchConsumerOps virtual_consumer_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_CONSUMER_OPS_ABI_VERSION,
		PgBatchConsumerOps),
	.accept_batch = virtual_accept_batch,
	.select_row = virtual_select_row,
};

PgBatchOutput *
pg_batch_output_create(MemoryContext parent_context, const PgBatchAPI *api,
					   TupleTableSlot *slot,
					   const AttrNumber *source_attnums, int ncolumns)
{
	PgBatchOutput *output;

	if (parent_context == NULL || api == NULL || slot == NULL)
		elog(ERROR, "pg_batch output requires a context, API, and slot");
	if (slot->tts_ops != &TTSOpsVirtual)
		elog(ERROR, "pg_batch output requires a virtual slot");
	if (ncolumns < slot->tts_tupleDescriptor->natts)
		elog(ERROR, "pg_batch output has %d columns but its slot needs %d",
			 ncolumns, slot->tts_tupleDescriptor->natts);
	output = MemoryContextAllocZero(parent_context, sizeof(*output));
	output->api = api;
	output->slot = slot;
	output->binding = api->attach_slot(slot, source_attnums, ncolumns,
									  &virtual_consumer_ops);
	return output;
}

PgBatchBinding *
pg_batch_output_binding(PgBatchOutput *output)
{
	return output->binding;
}

const PgBatchRequest *
pg_batch_output_request(PgBatchOutput *output)
{
	return output->api->get_request(output->binding);
}

static void
recycle_output(PgBatchOutput *output, bool require_finished)
{
	if (output->api->get_batch(output->binding) == NULL)
		return;
	if (require_finished && !output->api->batch_finished(output->binding))
		elog(ERROR, "pg_batch parent requested a new batch too early");
	output->api->clear_batch(output->binding);
	ExecClearTuple(output->slot);
}

TupleTableSlot *
pg_batch_output_publish(PgBatchOutput *output, PgBatch *batch)
{
	recycle_output(output, true);
	output->api->publish_batch(output->binding, batch);
	return output->slot;
}

TupleTableSlot *
pg_batch_output_select(PgBatchOutput *output, int row)
{
	output->api->select_row(output->binding, row);
	return output->slot;
}

void
pg_batch_output_finish(PgBatchOutput *output)
{
	output->api->finish_batch(output->binding);
}

void
pg_batch_output_reset(PgBatchOutput *output)
{
	recycle_output(output, false);
}
