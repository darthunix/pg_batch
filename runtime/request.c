#include "postgres.h"

#include "executor/executor.h"

#include "runtime.h"

struct PgBatchRequestPort
{
	const PgBatchAPI *api;
	PgBatchBinding *binding;
};

static void
request_port_accept_batch(TupleTableSlot *slot, PgBatch *batch)
{
	(void) slot;
	(void) batch;
	elog(ERROR, "pg_batch request port cannot accept a batch");
}

static void
request_port_select_row(TupleTableSlot *slot, PgBatch *batch, int row)
{
	(void) slot;
	(void) batch;
	(void) row;
	elog(ERROR, "pg_batch request port cannot select a row");
}

static const PgBatchConsumerOps request_port_consumer_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_CONSUMER_OPS_ABI_VERSION,
		PgBatchConsumerOps),
	.accept_batch = request_port_accept_batch,
	.select_row = request_port_select_row,
};

PgBatchRequestPort *
pg_batch_request_port_create(MemoryContext parent_context,
							 const PgBatchAPI *api, TupleTableSlot *slot,
							 const AttrNumber *source_attnums, int ncolumns)
{
	PgBatchRequestPort *port;

	if (parent_context == NULL || api == NULL || slot == NULL)
		elog(ERROR, "pg_batch request port requires a context, API, and slot");
	port = MemoryContextAllocZero(parent_context, sizeof(*port));
	port->api = api;
	port->binding = api->attach_slot(slot, source_attnums, ncolumns,
								 &request_port_consumer_ops);
	return port;
}

PgBatchBinding *
pg_batch_request_port_binding(PgBatchRequestPort *port)
{
	return port->binding;
}

const PgBatchRequest *
pg_batch_request_port_request(PgBatchRequestPort *port)
{
	return port->api->get_request(port->binding);
}

void
pg_batch_request_port_set_request(PgBatchRequestPort *port,
								  const Bitmapset *filter_columns,
								  const Bitmapset *project_columns,
								  bool return_batch)
{
	port->api->set_request(port->binding, filter_columns, project_columns,
						   return_batch);
}
