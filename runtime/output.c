#include "postgres.h"

#include "executor/executor.h"

#include "runtime.h"

struct PgBatchOutput
{
	const PgBatchAPI *api;
	TupleTableSlot *slot;
	PgBatchBinding *binding;
	int		   *batch_columns;
};

static void virtual_select_row(void *consumer_state, TupleTableSlot *slot,
	PgBatch *batch, int row);

static void
virtual_accept_batch(void *consumer_state, TupleTableSlot *slot,
					 PgBatch *batch)
{
	int			row = pg_batch_selection_next(&batch->selection, -1);

	if (row < 0)
		elog(ERROR, "pg_batch source published an empty selection");
	virtual_select_row(consumer_state, slot, batch, row);
}

static void
virtual_select_row(void *consumer_state, TupleTableSlot *slot,
				   PgBatch *batch, int row)
{
	PgBatchOutput *output = consumer_state;
	TupleDesc	desc = slot->tts_tupleDescriptor;
	uint64		local_rows;
	uint64	   *rows;
	PgBatchSelection selected;
	int			word = row / 64;
	uint64		bit = UINT64CONST(1) << (row % 64);

	Assert(slot->tts_ops == &TTSOpsVirtual);
	Assert(pg_batch_selection_contains(&batch->selection, row));
	ExecClearTuple(slot);
	if (batch->selection.nwords == 1)
	{
		local_rows = bit;
		rows = &local_rows;
	}
	else
	{
		rows = palloc0_array(uint64, batch->selection.nwords);
		rows[word] = bit;
	}
	selected.nrows = batch->selection.nrows;
	selected.nwords = batch->selection.nwords;
	selected.words = rows;
	for (int attribute = 0; attribute < desc->natts; attribute++)
	{
		PgBatchDatumVector vector;
		int			column = output->batch_columns[attribute];

		pg_batch_get_datum_column(batch, column, &selected,
								  PG_BATCH_COLUMN_PROJECT, &vector);
		slot->tts_values[attribute] = vector.values[row];
		slot->tts_isnull[attribute] = vector.isnull[row];
	}
	if (batch->selection.nwords > 1)
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
pg_batch_output_create(MemoryContext parent_context, TupleTableSlot *slot,
					   const PgBatchLayout *layout)
{
	PgBatchOutput *output;
	const PgBatchAPI *api = pg_batch_api_get();

	if (parent_context == NULL || slot == NULL || layout == NULL)
		elog(ERROR, "pg_batch output requires a context, slot, and layout");
	pg_batch_check_layout(layout);
	if (slot->tts_ops != &TTSOpsVirtual)
		elog(ERROR, "pg_batch output requires a virtual slot");
	if (layout->ntargets < slot->tts_tupleDescriptor->natts)
		elog(ERROR, "pg_batch output describes %d targets but its slot needs %d",
			 layout->ntargets, slot->tts_tupleDescriptor->natts);
	output = MemoryContextAllocZero(parent_context, sizeof(*output));
	output->api = api;
	output->slot = slot;
	output->batch_columns = MemoryContextAlloc(parent_context,
		sizeof(int) * slot->tts_tupleDescriptor->natts);
	for (int attribute = 0; attribute < slot->tts_tupleDescriptor->natts;
		 attribute++)
	{
		output->batch_columns[attribute] =
			pg_batch_layout_column(layout, attribute);
		if (output->batch_columns[attribute] < 0)
			elog(ERROR, "pg_batch output target %d has no batch column",
				 attribute + 1);
	}
	output->binding = api->attach_slot(slot, layout,
									  &virtual_consumer_ops, output);
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

void
pg_batch_output_set_request(PgBatchOutput *output,
							const PgBatchRequestSpec *spec)
{
	output->api->set_request(output->binding, spec);
}

bool
pg_batch_output_finished(PgBatchOutput *output)
{
	return output->api->finished(output->binding);
}

static void
recycle_output(PgBatchOutput *output, bool require_finished)
{
	if (output->api->get_batch(output->binding) == NULL)
		return;
	if (require_finished && !output->api->finished(output->binding))
		elog(ERROR, "pg_batch parent requested a new batch too early");
	output->api->clear(output->binding);
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
	output->api->finish(output->binding);
}

void
pg_batch_output_clear(PgBatchOutput *output)
{
	recycle_output(output, false);
}
