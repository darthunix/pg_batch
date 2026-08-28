#include "postgres.h"

#include "executor/executor.h"

#include "runtime.h"

struct PgBatchInput
{
	const PgBatchBridgeAPI *bridge;
	PlanState  *child;
	PgBatchBridgeBinding *request_binding;
	TupleTableSlot *cached_slot;
	PgBatchBridgeBinding *cached_binding;
	PgBatchBridgeBinding *active_binding;
	PgBatchBridgeBatch *active_batch;
};

PgBatchInput *
pg_batch_input_create(MemoryContext parent_context,
					  const PgBatchBridgeAPI *bridge,
					  PlanState *child, const char *producer_name)
{
	PgBatchInput *input;
	const PgBatchBridgeProducerOps *producer;

	if (parent_context == NULL || bridge == NULL || child == NULL ||
		producer_name == NULL || producer_name[0] == '\0')
		elog(ERROR, "pg_batch input requires a context, bridge, child, and producer");
	input = MemoryContextAllocZero(parent_context, sizeof(*input));
	input->bridge = bridge;
	input->child = child;
	producer = bridge->get_producer(producer_name);
	if (producer == NULL)
		elog(ERROR, "pg_batch producer \"%s\" is not registered", producer_name);
	input->request_binding = producer->get_request_binding(child);
	if (input->request_binding == NULL)
		elog(ERROR, "pg_batch producer \"%s\" has no request binding",
			 producer_name);
	return input;
}

PgBatchBridgeBinding *
pg_batch_input_request_binding(PgBatchInput *input)
{
	return input->request_binding;
}

static bool
input_fetch(PgBatchInput *input, PgBatchInputBatch *result)
{
	TupleTableSlot *slot;
	PgBatchBridgeBatch *batch;

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
		input->cached_binding = input->bridge->find_binding(slot);
	}
	if (input->cached_binding == NULL)
		elog(ERROR, "pg_batch producer returned a slot without a batch binding");
	batch = input->bridge->get_batch(input->cached_binding);
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
	if (unlikely(input->active_batch != NULL))
		elog(ERROR, "pg_batch input requested a new batch before finishing the previous one");
	return input_fetch(input, result);
}

bool
pg_batch_input_advance(PgBatchInput *input, PgBatchInputBatch *result)
{
	if (input->active_batch != NULL)
	{
		/* The bridge finish operation is exactly this hot state transition. */
		input->active_batch->consumed = true;
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
	input->bridge->finish_batch(input->active_binding);
	input->active_binding = NULL;
	input->active_batch = NULL;
}

void
pg_batch_input_reset(PgBatchInput *input)
{
	input->cached_slot = NULL;
	input->cached_binding = NULL;
	input->active_binding = NULL;
	input->active_batch = NULL;
}
