#include "postgres.h"

#include "executor/executor.h"

#include "runtime.h"
#include "node.h"

struct PgBatchInput
{
	const PgBatchAPI *api;
	PlanState  *child;
	PgBatchBinding *request_binding;
	TupleTableSlot *cached_slot;
	PgBatchBinding *cached_binding;
	TupleTableSlot *active_slot;
	PgBatchBinding *active_binding;
	PgBatch *active_batch;
	PgBatchRowView row_view;
	bool		has_row_view;
};

PgBatchInput *
pg_batch_input_create(MemoryContext parent_context,
					  const PgBatchAPI *api,
					  PlanState *child, const char *producer_name)
{
	PgBatchInput *input;
	const PgBatchProducerOps *producer;

	if (parent_context == NULL || api == NULL || child == NULL ||
		producer_name == NULL || producer_name[0] == '\0')
		elog(ERROR, "pg_batch input requires a context, API, child, and producer");
	input = MemoryContextAllocZero(parent_context, sizeof(*input));
	input->api = api;
	input->child = child;
	producer = api->get_producer(producer_name);
	if (producer == NULL)
		elog(ERROR, "pg_batch producer \"%s\" is not registered", producer_name);
	input->request_binding = producer->get_request_binding(child);
	if (input->request_binding == NULL)
		elog(ERROR, "pg_batch producer \"%s\" has no request binding",
			 producer_name);
	return input;
}

PgBatchBinding *
pg_batch_input_request_binding(PgBatchInput *input)
{
	return input->request_binding;
}

void
pg_batch_input_forward_request(PgBatchInput *input,
							   const PgBatchRequest *request,
							   bool return_batch)
{
	const PgBatchRequest *child_request;
	Bitmapset  *filter_columns;
	Bitmapset  *project_columns;

	if (request == NULL || request->struct_size < PG_BATCH_REQUEST_MIN_SIZE)
		elog(ERROR, "pg_batch input received an incompatible request");
	child_request = input->api->get_request(input->request_binding);
	if (child_request == NULL ||
		child_request->struct_size < PG_BATCH_REQUEST_MIN_SIZE)
		elog(ERROR, "pg_batch producer returned an incompatible request");
	filter_columns = bms_union(child_request->filter_columns,
		request->filter_columns);
	project_columns = bms_union(child_request->project_columns,
		request->project_columns);
	input->api->set_request(input->request_binding, filter_columns,
		project_columns, return_batch);
	bms_free(filter_columns);
	bms_free(project_columns);
}

static bool
input_fetch(PgBatchInput *input, PgBatchInputBatch *result)
{
	TupleTableSlot *slot;
	PgBatch *batch;

	if (result == NULL)
		elog(ERROR, "pg_batch input requires a result");

	result->slot = NULL;
	result->batch = NULL;
	slot = ExecProcNode(input->child);
	if (TupIsNull(slot))
		return false;
	if (slot != input->cached_slot)
	{
		input->cached_slot = slot;
		input->cached_binding = input->api->find_binding(slot);
	}
	if (input->cached_binding == NULL)
		elog(ERROR, "pg_batch producer returned a slot without a batch binding");
	batch = input->api->get_batch(input->cached_binding);
	if (batch == NULL)
		elog(ERROR, "pg_batch producer returned no batch");

	input->active_binding = input->cached_binding;
	input->active_slot = slot;
	input->active_batch = batch;
	input->has_row_view = false;
	if (PG_BATCH_ABI_HAS_FIELD(input->api, PgBatchAPI, get_row_view) &&
		input->api->get_row_view != NULL)
	{
		input->row_view = (PgBatchRowView) PG_BATCH_STRUCT_INITIALIZER(
			PgBatchRowView);
		input->api->get_row_view(input->active_binding, &input->row_view);
		input->has_row_view = true;
	}
	result->slot = slot;
	result->batch = batch;
	return true;
}

bool
pg_batch_input_next(PgBatchInput *input, PgBatchInputBatch *result)
{
	if (input->active_batch != NULL)
	{
		/* A pass-through parent may have finished this binding directly. */
		if (!input->api->batch_finished(input->active_binding))
			elog(ERROR, "pg_batch input requested a new batch before finishing the previous one");
		input->active_binding = NULL;
		input->active_slot = NULL;
		input->active_batch = NULL;
		input->has_row_view = false;
	}
	return input_fetch(input, result);
}

TupleTableSlot *
pg_batch_input_select_row(PgBatchInput *input, int row)
{
	if (unlikely(input->active_batch == NULL))
		elog(ERROR, "pg_batch input has no active batch");
	if (likely(input->has_row_view))
		return pg_batch_row_view_select(&input->row_view, row);
	input->api->select_row(input->active_binding, row);
	return input->active_slot;
}

const PgBatchRowView *
pg_batch_input_row_view(PgBatchInput *input)
{
	if (unlikely(input->active_batch == NULL))
		elog(ERROR, "pg_batch input has no active batch");
	return input->has_row_view ? &input->row_view : NULL;
}

TupleTableSlot *
pg_batch_input_copy_row(PgBatchInput *input, int row,
						TupleTableSlot *destination,
						const int *batch_columns)
{
	PgBatch    *batch = input->active_batch;
	TupleDesc	desc;
	TupleTableSlot *source;
	uint64		local_rows;
	uint64	   *rows;
	int			word;
	uint64		bit;

	if (unlikely(batch == NULL))
		elog(ERROR, "pg_batch input has no active batch");
	if (destination == NULL || destination->tts_ops != &TTSOpsVirtual ||
		batch_columns == NULL)
		elog(ERROR, "pg_batch input requires a virtual mapped destination");
	source = pg_batch_input_select_row(input, row);
	desc = destination->tts_tupleDescriptor;
	word = row / 64;
	bit = UINT64CONST(1) << (row % 64);
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

	ExecClearTuple(destination);
	for (int attribute = 0; attribute < desc->natts; attribute++)
	{
		PgBatchDatumVector vector;

		pg_batch_get_datum_column(batch, batch_columns[attribute], rows,
			PG_BATCH_COLUMN_PROJECT, &vector);
		if (word >= vector.nwords || (vector.valid_rows[word] & bit) == 0)
			elog(ERROR, "pg_batch input did not materialize column %d row %d",
				 batch_columns[attribute] + 1, row);
		destination->tts_values[attribute] = vector.values[row];
		destination->tts_isnull[attribute] = vector.isnull[row];
	}
	if (batch->nwords > 1)
		pfree(rows);
	destination->tts_tid = source->tts_tid;
	destination->tts_tableOid = batch->table_oid;
	ExecStoreVirtualTuple(destination);
	return destination;
}

bool
pg_batch_input_batch_finished(PgBatchInput *input)
{
	if (unlikely(input->active_batch == NULL))
		elog(ERROR, "pg_batch input has no active batch");
	return input->api->batch_finished(input->active_binding);
}

void
pg_batch_input_finish(PgBatchInput *input)
{
	if (unlikely(input->active_batch == NULL))
		elog(ERROR, "pg_batch input has no active batch");
	input->api->finish_batch(input->active_binding);
	input->active_slot = NULL;
	input->active_binding = NULL;
	input->active_batch = NULL;
	input->has_row_view = false;
}

void
pg_batch_input_rescan(PgBatchInput *input)
{
	input->cached_slot = NULL;
	input->cached_binding = NULL;
	input->active_slot = NULL;
	input->active_binding = NULL;
	input->active_batch = NULL;
	input->has_row_view = false;
}
