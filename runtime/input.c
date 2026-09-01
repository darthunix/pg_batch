#include "postgres.h"

#include "executor/executor.h"

#include "runtime.h"
#include "node.h"

struct PgBatchInput
{
	const PgBatchAPI *api;
	PlanState  *child;
	PgBatchBinding *request_binding;
	const PgBatchRequest *request;
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
					  PlanState *child, const char *node_name)
{
	PgBatchInput *input;
	const PgBatchNodeOps *node;
	const PgBatchRequest *request;

	if (parent_context == NULL || child == NULL ||
		node_name == NULL || node_name[0] == '\0')
		elog(ERROR, "pg_batch input requires a context, child, and node");
	input = MemoryContextAllocZero(parent_context, sizeof(*input));
	input->api = pg_batch_api_get();
	input->child = child;
	node = input->api->get_node(node_name);
	if (node == NULL)
		elog(ERROR, "pg_batch node \"%s\" is not registered", node_name);
	input->request_binding = node->get_request_binding(child);
	if (input->request_binding == NULL)
		elog(ERROR, "pg_batch node \"%s\" has no request binding",
			 node_name);
	input->request = input->api->get_request(input->request_binding);
	request = input->request;
	if (request == NULL || request->struct_size < PG_BATCH_REQUEST_MIN_SIZE)
		elog(ERROR, "pg_batch node \"%s\" returned an invalid request",
			 node_name);
	pg_batch_check_layout(request->layout);
	return input;
}

const PgBatchLayout *
pg_batch_input_layout(PgBatchInput *input)
{
	return input->request->layout;
}

PgBatchBinding *
pg_batch_input_request_binding(PgBatchInput *input)
{
	return input->request_binding;
}

const PgBatchRequest *
pg_batch_input_request(PgBatchInput *input)
{
	return input->request;
}

void
pg_batch_input_set_request(PgBatchInput *input,
						   const PgBatchRequestSpec *spec)
{
	input->api->set_request(input->request_binding, spec);
}

static bool
input_fetch(PgBatchInput *input, PgBatchInputResult *result)
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
		elog(ERROR, "pg_batch node returned a slot without a batch binding");
	batch = input->api->get_batch(input->cached_binding);
	if (batch == NULL)
		elog(ERROR, "pg_batch node returned no batch");

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
pg_batch_input_next(PgBatchInput *input, PgBatchInputResult *result)
{
	if (input->active_batch != NULL)
	{
		/* A forwarding parent may have finished this binding directly. */
		if (!input->api->finished(input->active_binding))
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
	PgBatchSelection selected;
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

	ExecClearTuple(destination);
	for (int attribute = 0; attribute < desc->natts; attribute++)
	{
		PgBatchDatumVector vector;

		pg_batch_get_datum_column(batch, batch_columns[attribute], &selected,
			PG_BATCH_COLUMN_PROJECT, &vector);
		destination->tts_values[attribute] = vector.values[row];
		destination->tts_isnull[attribute] = vector.isnull[row];
	}
	if (batch->selection.nwords > 1)
		pfree(rows);
	destination->tts_tid = source->tts_tid;
	destination->tts_tableOid = batch->table_oid;
	ExecStoreVirtualTuple(destination);
	return destination;
}

bool
pg_batch_input_finished(PgBatchInput *input)
{
	if (unlikely(input->active_batch == NULL))
		elog(ERROR, "pg_batch input has no active batch");
	return input->api->finished(input->active_binding);
}

void
pg_batch_input_finish(PgBatchInput *input)
{
	if (unlikely(input->active_batch == NULL))
		elog(ERROR, "pg_batch input has no active batch");
	input->api->finish(input->active_binding);
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
