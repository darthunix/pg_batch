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
	PgBatchBinding *active_binding;
	PgBatch *active_batch;
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
	input->active_batch = batch;
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
		input->active_batch = NULL;
	}
	return input_fetch(input, result);
}

void
pg_batch_input_finish(PgBatchInput *input)
{
	if (unlikely(input->active_batch == NULL))
		elog(ERROR, "pg_batch input has no active batch");
	input->api->finish_batch(input->active_binding);
	input->active_binding = NULL;
	input->active_batch = NULL;
}

void
pg_batch_input_rescan(PgBatchInput *input)
{
	input->cached_slot = NULL;
	input->cached_binding = NULL;
	input->active_binding = NULL;
	input->active_batch = NULL;
}
