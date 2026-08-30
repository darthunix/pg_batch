#include "postgres.h"

#include "fmgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "bridge.h"
#include "node.h"
#include "source.h"

PG_MODULE_MAGIC;

PGDLLEXPORT void _PG_init(void);

typedef struct SlotKey
{
	TupleTableSlot *slot;
} SlotKey;

typedef struct SlotEntry
{
	SlotKey		key;
	PgBatchBinding *binding;
} SlotEntry;

struct PgBatchBinding
{
	TupleTableSlot *slot;
	MemoryContext owner;
	MemoryContextCallback cleanup;
	PgBatchRequest request;
	const PgBatchConsumerOps *consumer_ops;
	PgBatch *batch;
	const PgBatchOps *validated_ops;
	bool		batch_finished;
};

static HTAB *slot_entries;
static List *providers;
static List *producers;

static void register_provider(const PgBatchProviderOps *provider);
static void unregister_provider(const char *provider_name);
static const PgBatchProviderOps *find_provider(Relation relation);
static const PgBatchProviderOps *get_provider(const char *provider_name);
static void register_producer(const PgBatchProducerOps *producer);
static void unregister_producer(const char *producer_name);
static const PgBatchProducerOps *find_producer(const Path *path);
static const PgBatchProducerOps *get_producer(const char *producer_name);
static PgBatchBinding *find_binding(TupleTableSlot *slot);
static PgBatchBinding *attach_slot(TupleTableSlot *slot,
										 const AttrNumber *source_attnums,
										 int ncolumns,
										 const PgBatchConsumerOps *consumer_ops);
static void set_request(PgBatchBinding *binding,
						const Bitmapset *filter_columns,
						const Bitmapset *project_columns,
						bool return_batch);
static void set_provider(PgBatchBinding *binding,
						 const char *provider_name,
						 void *provider_state);
static const PgBatchRequest *get_request(PgBatchBinding *binding);
static void publish_batch(PgBatchBinding *binding,
						  PgBatch *batch);
static PgBatch *get_batch(PgBatchBinding *binding);
static void select_row(PgBatchBinding *binding, int row);
static void finish_batch(PgBatchBinding *binding);
static bool batch_finished(PgBatchBinding *binding);
static void clear_batch(PgBatchBinding *binding);
static void detach_binding(PgBatchBinding *binding);

static const PgBatchAPI bridge_api = {
	.abi_version = PG_BATCH_ABI_VERSION,
	.struct_size = sizeof(PgBatchAPI),
	.register_provider = register_provider,
	.unregister_provider = unregister_provider,
	.find_provider = find_provider,
	.get_provider = get_provider,
	.register_producer = register_producer,
	.unregister_producer = unregister_producer,
	.find_producer = find_producer,
	.get_producer = get_producer,
	.find_binding = find_binding,
	.attach_slot = attach_slot,
	.set_request = set_request,
	.set_provider = set_provider,
	.get_request = get_request,
	.publish_batch = publish_batch,
	.get_batch = get_batch,
	.select_row = select_row,
	.finish_batch = finish_batch,
	.batch_finished = batch_finished,
	.clear_batch = clear_batch,
	.detach_binding = detach_binding,
};

static void
init_slots(void)
{
	HASHCTL		ctl;
	MemoryContext oldcontext;

	if (slot_entries != NULL)
		return;
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(SlotKey);
	ctl.entrysize = sizeof(SlotEntry);
	ctl.hcxt = TopMemoryContext;
	slot_entries = hash_create("pg_batch slot attachments", 32, &ctl,
							   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	MemoryContextSwitchTo(oldcontext);
}

static void
release_batch(PgBatchBinding *binding)
{
	PgBatch *batch = binding->batch;

	if (batch == NULL)
		return;
	binding->batch = NULL;
	binding->batch_finished = false;
	if (batch->ops->release != NULL)
		batch->ops->release(batch);
}

static void
slot_reset(void *arg)
{
	PgBatchBinding *binding = arg;
	SlotKey		key = {binding->slot};
	SlotEntry  *entry;

	if (slot_entries == NULL)
		return;
	entry = hash_search(slot_entries, &key, HASH_FIND, NULL);
	if (entry == NULL)
		return;

	/*
	 * The slot owner may delete child contexts that own the batch before this
	 * callback runs. Remove the weak attachment only; source resources must
	 * also be protected by PostgreSQL resource owners or their own context
	 * callback.
	 */
	hash_search(slot_entries, &key, HASH_REMOVE, NULL);
}

static void
validate_abi(uint32 version, Size struct_size,
			 const char *what, Size required_size)
{
	if (version != PG_BATCH_ABI_VERSION || struct_size < required_size)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("incompatible %s ABI", what),
				 errdetail("Expected version %u and at least %zu bytes, got version %u and %zu bytes.",
						   PG_BATCH_ABI_VERSION, required_size,
						   version, struct_size)));
}

static void
register_provider(const PgBatchProviderOps *provider)
{
	MemoryContext oldcontext;

	if (provider == NULL || provider->provider_name == NULL ||
		provider->provider_name[0] == '\0')
		elog(ERROR, "pg_batch provider must have a name");
	validate_abi(provider->abi_version, provider->struct_size,
				 "provider", sizeof(PgBatchProviderOps));
	if (provider->supports_relation == NULL || provider->plan_scan == NULL ||
		provider->begin_scan == NULL || provider->rescan == NULL ||
		provider->end_scan == NULL)
		elog(ERROR, "pg_batch provider \"%s\" has incomplete operations",
			 provider->provider_name);
	foreach_ptr(const PgBatchProviderOps, existing, providers)
	{
		if (strcmp(existing->provider_name, provider->provider_name) == 0)
		{
			if (existing == provider)
				return;
			elog(ERROR, "pg_batch provider \"%s\" is already registered",
				 provider->provider_name);
		}
	}
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	providers = lappend(providers, (void *) provider);
	MemoryContextSwitchTo(oldcontext);
}

static void
unregister_provider(const char *provider_name)
{
	ListCell   *lc;

	foreach(lc, providers)
	{
		const PgBatchProviderOps *provider = lfirst(lc);

		if (strcmp(provider->provider_name, provider_name) == 0)
		{
			providers = foreach_delete_current(providers, lc);
			return;
		}
	}
}

static const PgBatchProviderOps *
find_provider(Relation relation)
{
	const PgBatchProviderOps *result = NULL;

	foreach_ptr(const PgBatchProviderOps, provider, providers)
	{
		if (!provider->supports_relation(relation))
			continue;
		if (result != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("multiple pg_batch providers support relation \"%s\"",
							RelationGetRelationName(relation)),
					 errdetail("Providers \"%s\" and \"%s\" both claimed the relation.",
							   result->provider_name, provider->provider_name)));
		result = provider;
	}
	return result;
}

static const PgBatchProviderOps *
get_provider(const char *provider_name)
{
	foreach_ptr(const PgBatchProviderOps, provider, providers)
	{
		if (strcmp(provider->provider_name, provider_name) == 0)
			return provider;
	}
	return NULL;
}

static void
register_producer(const PgBatchProducerOps *producer)
{
	MemoryContext oldcontext;

	if (producer == NULL || producer->producer_name == NULL ||
		producer->producer_name[0] == '\0' ||
		producer->supports_path == NULL ||
		producer->get_output_layout == NULL ||
		producer->get_request_binding == NULL)
		elog(ERROR, "invalid pg_batch producer registration");
	validate_abi(producer->abi_version, producer->struct_size,
				 "producer", sizeof(PgBatchProducerOps));
	foreach_ptr(const PgBatchProducerOps, existing, producers)
	{
		if (strcmp(existing->producer_name, producer->producer_name) == 0)
		{
			if (existing == producer)
				return;
			elog(ERROR, "pg_batch producer \"%s\" is already registered",
				 producer->producer_name);
		}
	}
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	producers = lappend(producers, (void *) producer);
	MemoryContextSwitchTo(oldcontext);
}

static void
unregister_producer(const char *producer_name)
{
	const PgBatchProducerOps *producer = get_producer(producer_name);

	if (producer != NULL)
		producers = list_delete_ptr(producers, (void *) producer);
}

static const PgBatchProducerOps *
find_producer(const Path *path)
{
	const PgBatchProducerOps *result = NULL;

	foreach_ptr(const PgBatchProducerOps, producer, producers)
	{
		if (!producer->supports_path(path))
			continue;
		if (result != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("multiple pg_batch producers support one plan path"),
					 errdetail("Producers \"%s\" and \"%s\" both claimed the path.",
							   result->producer_name, producer->producer_name)));
		result = producer;
	}
	return result;
}

static const PgBatchProducerOps *
get_producer(const char *producer_name)
{
	foreach_ptr(const PgBatchProducerOps, producer, producers)
	{
		if (strcmp(producer->producer_name, producer_name) == 0)
			return producer;
	}
	return NULL;
}

static PgBatchBinding *
find_binding(TupleTableSlot *slot)
{
	SlotKey		key = {slot};
	SlotEntry  *entry;

	if (slot_entries == NULL)
		return NULL;
	entry = hash_search(slot_entries, &key, HASH_FIND, NULL);
	return entry == NULL ? NULL : entry->binding;
}

static PgBatchBinding *
attach_slot(TupleTableSlot *slot,
			const AttrNumber *source_attnums, int ncolumns,
			const PgBatchConsumerOps *consumer_ops)
{
	SlotKey		key = {slot};
	SlotEntry  *entry;
	PgBatchBinding *binding;
	AttrNumber *attnums = NULL;
	bool		found;

	if (slot == NULL || ncolumns < 0 ||
		(ncolumns > 0 && source_attnums == NULL))
		elog(ERROR, "invalid pg_batch slot configuration");
	if (consumer_ops == NULL || consumer_ops->accept_batch == NULL ||
		consumer_ops->select_row == NULL)
		elog(ERROR, "pg_batch slot requires consumer operations");
	validate_abi(consumer_ops->abi_version,
				 consumer_ops->struct_size, "consumer",
				 sizeof(PgBatchConsumerOps));
	init_slots();
	entry = hash_search(slot_entries, &key, HASH_ENTER, &found);
	if (found)
		elog(ERROR, "pg_batch slot is already configured");
	binding = MemoryContextAllocZero(slot->tts_mcxt, sizeof(*binding));
	binding->slot = slot;
	binding->owner = slot->tts_mcxt;
	entry->binding = binding;
	if (ncolumns > 0)
	{
		attnums = MemoryContextAlloc(slot->tts_mcxt,
									 sizeof(AttrNumber) * ncolumns);
		memcpy(attnums, source_attnums, sizeof(AttrNumber) * ncolumns);
	}
	binding->request.source_attnums = attnums;
	binding->request.ncolumns = ncolumns;
	binding->consumer_ops = consumer_ops;
	binding->cleanup.func = slot_reset;
	binding->cleanup.arg = binding;
	MemoryContextRegisterResetCallback(slot->tts_mcxt,
									   &binding->cleanup);
	return binding;
}

static void
set_request(PgBatchBinding *binding,
			const Bitmapset *filter_columns,
			const Bitmapset *project_columns,
			bool return_batch)
{
	MemoryContext oldcontext;
	Bitmapset  *new_filter;
	Bitmapset  *new_survivor;
	int			column;

	if (binding == NULL)
		elog(ERROR, "pg_batch cannot configure a null binding");
	column = bms_next_member(filter_columns, binding->request.ncolumns - 1);
	if (column >= 0)
		elog(ERROR, "pg_batch filter column %d is out of range", column);
	column = bms_next_member(project_columns, binding->request.ncolumns - 1);
	if (column >= 0)
		elog(ERROR, "pg_batch project column %d is out of range", column);

	oldcontext = MemoryContextSwitchTo(binding->owner);
	new_filter = bms_copy(filter_columns);
	new_survivor = bms_copy(project_columns);
	MemoryContextSwitchTo(oldcontext);

	bms_free((Bitmapset *) binding->request.filter_columns);
	bms_free((Bitmapset *) binding->request.project_columns);
	binding->request.filter_columns = new_filter;
	binding->request.project_columns = new_survivor;
	binding->request.return_batch = return_batch;
}

static void
set_provider(PgBatchBinding *binding,
			 const char *provider_name, void *provider_state)
{
	binding->request.provider_name = provider_name;
	binding->request.provider_state = provider_state;
}

static const PgBatchRequest *
get_request(PgBatchBinding *binding)
{
	return binding == NULL ? NULL : &binding->request;
}

static void
publish_batch(PgBatchBinding *binding,
			  PgBatch *batch)
{
	if (binding->batch != NULL)
		elog(ERROR, "pg_batch source published a new batch before clearing the previous one");
	if (batch == NULL || batch->ops == NULL || batch->selection == NULL ||
		batch->nrows <= 0 || batch->nwords != (batch->nrows + 63) / 64)
		elog(ERROR, "pg_batch source published an invalid batch");
	if (batch->nrows % 64 != 0 &&
		(batch->selection[batch->nwords - 1] &
		 (UINT64_MAX << (batch->nrows % 64))) != 0)
		elog(ERROR, "pg_batch source selected rows beyond the end of its batch");
	validate_abi(batch->abi_version, batch->struct_size,
				 "batch", sizeof(PgBatch));
	if (batch->ops != binding->validated_ops)
	{
		validate_abi(batch->ops->abi_version,
					 batch->ops->struct_size, "batch operations",
					 sizeof(PgBatchOps));
		if (batch->ops->get_datum_column == NULL)
			elog(ERROR, "pg_batch source must provide Datum materialization");
		binding->validated_ops = batch->ops;
	}
	binding->batch = batch;
	binding->batch_finished = false;
	binding->consumer_ops->accept_batch(binding->slot, batch);
}

static PgBatch *
get_batch(PgBatchBinding *binding)
{
	return binding == NULL ? NULL : binding->batch;
}

static void
select_row(PgBatchBinding *binding, int row)
{
	if (binding->batch == NULL || row < 0 || row >= binding->batch->nrows ||
		!pg_batch_row_selected(binding->batch, row))
		elog(ERROR, "pg_batch cannot select row %d", row);
	binding->consumer_ops->select_row(binding->slot, binding->batch, row);
}

static void
finish_batch(PgBatchBinding *binding)
{
	if (binding->batch == NULL)
		elog(ERROR, "pg_batch cannot finish a slot without an active batch");
	binding->batch_finished = true;
}

static bool
batch_finished(PgBatchBinding *binding)
{
	if (binding == NULL)
		elog(ERROR, "pg_batch cannot inspect a null binding");
	return binding->batch == NULL || binding->batch_finished;
}

static void
clear_batch(PgBatchBinding *binding)
{
	if (binding != NULL)
		release_batch(binding);
}

static void
detach_binding(PgBatchBinding *binding)
{
	SlotKey		key;

	if (binding == NULL)
		return;
	key.slot = binding->slot;
	release_batch(binding);
	MemoryContextUnregisterResetCallback(binding->owner,
										 &binding->cleanup);
	bms_free((Bitmapset *) binding->request.filter_columns);
	bms_free((Bitmapset *) binding->request.project_columns);
	if (binding->request.source_attnums != NULL)
		pfree((void *) binding->request.source_attnums);
	hash_search(slot_entries, &key, HASH_REMOVE, NULL);
	pfree(binding);
}

void
_PG_init(void)
{
	void	  **rendezvous = find_rendezvous_variable(PG_BATCH_RENDEZVOUS);

	if (*rendezvous != NULL && *rendezvous != &bridge_api)
		elog(ERROR, "pg_batch bridge rendezvous variable is already in use");
	*rendezvous = (void *) &bridge_api;
}
