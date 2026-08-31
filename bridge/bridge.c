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
	PgBatchLayout *layout;
	const PgBatchConsumerOps *consumer_ops;
	const char *provider_name;
	void	   *provider_state;
	PgBatch *batch;
	const PgBatchOps *validated_ops;
	bool		request_sealed;
	bool		batch_finished;
};

static HTAB *slot_entries;
static List *providers;
static List *producers;
static uint64 next_batch_generation = 1;

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
									 const PgBatchLayout *layout,
									 const PgBatchConsumerOps *consumer_ops);
static void set_request(PgBatchBinding *binding,
						const Bitmapset *filter_columns,
						const Bitmapset *project_columns,
						bool return_batch,
						int max_rows);
static void set_provider(PgBatchBinding *binding,
						 const char *provider_name,
						 void *provider_state);
static void *get_bound_provider(PgBatchBinding *binding,
								const char **provider_name);
static const PgBatchRequest *seal_request(PgBatchBinding *binding);
static const PgBatchRequest *get_request(PgBatchBinding *binding);
static void publish_batch(PgBatchBinding *binding,
						  PgBatch *batch);
static PgBatch *get_batch(PgBatchBinding *binding);
static void select_row(PgBatchBinding *binding, int row);
static void finish_batch(PgBatchBinding *binding);
static bool batch_finished(PgBatchBinding *binding);
static void clear_batch(PgBatchBinding *binding);
static void detach_binding(PgBatchBinding *binding);
static void get_row_view(PgBatchBinding *binding, PgBatchRowView *result);

static const PgBatchAPI bridge_api = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_API_ABI_VERSION, PgBatchAPI),
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
	.get_bound_provider = get_bound_provider,
	.seal_request = seal_request,
	.get_request = get_request,
	.publish_batch = publish_batch,
	.get_batch = get_batch,
	.select_row = select_row,
	.finish_batch = finish_batch,
	.batch_finished = batch_finished,
	.clear_batch = clear_batch,
	.detach_binding = detach_binding,
	.get_row_view = get_row_view,
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
	batch->generation = 0;
	if (PG_BATCH_ABI_HAS_FIELD(batch->ops, PgBatchOps, release) &&
		batch->ops->release != NULL)
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
validate_abi(uint32 version, Size struct_size, uint32 expected_version,
			 const char *what, Size required_size)
{
	if (version != expected_version || struct_size < required_size)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("incompatible %s ABI", what),
				 errdetail("Expected version %u and at least %zu bytes, got version %u and %zu bytes.",
						   expected_version, required_size,
						   version, struct_size)));
}

static void
register_provider(const PgBatchProviderOps *provider)
{
	MemoryContext oldcontext;

	if (provider == NULL)
		elog(ERROR, "pg_batch cannot register a null provider");
	validate_abi(provider->abi_version, provider->struct_size,
				 PG_BATCH_PROVIDER_OPS_ABI_VERSION,
				 "provider", PG_BATCH_PROVIDER_OPS_MIN_SIZE);
	if (provider->provider_name == NULL || provider->provider_name[0] == '\0')
		elog(ERROR, "pg_batch provider must have a name");
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

	if (producer == NULL)
		elog(ERROR, "pg_batch cannot register a null producer");
	validate_abi(producer->abi_version, producer->struct_size,
				 PG_BATCH_PRODUCER_OPS_ABI_VERSION,
				 "producer", PG_BATCH_PRODUCER_OPS_MIN_SIZE);
	if (producer->producer_name == NULL || producer->producer_name[0] == '\0' ||
		producer->supports_path == NULL ||
		producer->get_layout == NULL ||
		producer->get_request_binding == NULL)
		elog(ERROR, "invalid pg_batch producer registration");
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
			const PgBatchLayout *layout,
			const PgBatchConsumerOps *consumer_ops)
{
	SlotKey		key = {slot};
	SlotEntry  *entry;
	PgBatchBinding *binding;
	int		   *target_columns = NULL;
	bool		found;

	if (slot == NULL)
		elog(ERROR, "invalid pg_batch slot configuration");
	pg_batch_check_layout(layout);
	if (consumer_ops == NULL)
		elog(ERROR, "pg_batch slot requires consumer operations");
	validate_abi(consumer_ops->abi_version,
				 consumer_ops->struct_size,
				 PG_BATCH_CONSUMER_OPS_ABI_VERSION,
				 "consumer", PG_BATCH_CONSUMER_OPS_MIN_SIZE);
	if (consumer_ops->accept_batch == NULL || consumer_ops->select_row == NULL)
		elog(ERROR, "pg_batch slot requires consumer operations");
	init_slots();
	entry = hash_search(slot_entries, &key, HASH_ENTER, &found);
	if (found)
		elog(ERROR, "pg_batch slot is already configured");
	binding = MemoryContextAllocZero(slot->tts_mcxt, sizeof(*binding));
	binding->slot = slot;
	binding->owner = slot->tts_mcxt;
	entry->binding = binding;
	binding->layout = MemoryContextAlloc(slot->tts_mcxt,
										 sizeof(*binding->layout));
	*binding->layout = *layout;
	binding->layout->struct_size = sizeof(*binding->layout);
	if (layout->target_columns != NULL)
	{
		target_columns = MemoryContextAlloc(slot->tts_mcxt,
										  sizeof(int) * layout->ntargets);
		memcpy(target_columns, layout->target_columns,
			   sizeof(int) * layout->ntargets);
	}
	binding->layout->target_columns = target_columns;
	binding->request.struct_size = sizeof(PgBatchRequest);
	binding->request.layout = binding->layout;
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
			bool return_batch,
			int max_rows)
{
	MemoryContext oldcontext;
	Bitmapset  *new_filter;
	Bitmapset  *new_survivor;
	int			column;

	if (binding == NULL)
		elog(ERROR, "pg_batch cannot configure a null binding");
	if (binding->request_sealed)
		elog(ERROR, "pg_batch request is already sealed");
	if (max_rows < 0)
		elog(ERROR, "pg_batch maximum row count cannot be negative");
	column = bms_next_member(filter_columns,
							 binding->layout->ncolumns - 1);
	if (column >= 0)
		elog(ERROR, "pg_batch filter column %d is out of range", column);
	column = bms_next_member(project_columns,
							 binding->layout->ncolumns - 1);
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
	binding->request.max_rows = max_rows;
}

static void
set_provider(PgBatchBinding *binding,
			 const char *provider_name, void *provider_state)
{
	if (binding == NULL)
		elog(ERROR, "pg_batch cannot configure a null binding");
	binding->provider_name = provider_name;
	binding->provider_state = provider_state;
}

static void *
get_bound_provider(PgBatchBinding *binding, const char **provider_name)
{
	if (provider_name != NULL)
		*provider_name = binding == NULL ? NULL : binding->provider_name;
	return binding == NULL ? NULL : binding->provider_state;
}

static const PgBatchRequest *
seal_request(PgBatchBinding *binding)
{
	if (binding == NULL)
		return NULL;
	binding->request_sealed = true;
	return &binding->request;
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
	uint64		generation;

	if (binding->batch != NULL)
		elog(ERROR, "pg_batch source published a new batch before clearing the previous one");
	if (batch == NULL)
		elog(ERROR, "pg_batch source published an invalid batch");
	validate_abi(batch->abi_version, batch->struct_size,
				 PG_BATCH_ABI_VERSION, "batch", PG_BATCH_MIN_SIZE);
	if (batch->ops == NULL || batch->selection == NULL ||
		batch->nrows <= 0 || batch->nwords != (batch->nrows + 63) / 64)
		elog(ERROR, "pg_batch source published an invalid batch");
	if (batch->generation != 0)
		elog(ERROR, "pg_batch source published a batch that is already active");
	if (binding->request.max_rows > 0 &&
		batch->nrows > binding->request.max_rows)
		elog(ERROR, "pg_batch source exceeded the requested batch size");
	if (batch->nrows % 64 != 0 &&
		(batch->selection[batch->nwords - 1] &
		 (UINT64_MAX << (batch->nrows % 64))) != 0)
		elog(ERROR, "pg_batch source selected rows beyond the end of its batch");
	if (batch->ops != binding->validated_ops)
	{
		validate_abi(batch->ops->abi_version,
					 batch->ops->struct_size,
					 PG_BATCH_OPS_ABI_VERSION,
					 "batch operations", PG_BATCH_OPS_MIN_SIZE);
		if (batch->ops->get_datum_column == NULL)
			elog(ERROR, "pg_batch source must provide Datum materialization");
		binding->validated_ops = batch->ops;
	}
	binding->request_sealed = true;
	generation = next_batch_generation++;
	if (generation == 0)
	{
		generation = next_batch_generation++;
		if (generation == 0)
			elog(ERROR, "pg_batch generation counter wrapped around");
	}
	batch->generation = generation;
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
	if (binding->batch == NULL || binding->batch_finished ||
		row < 0 || row >= binding->batch->nrows ||
		!pg_batch_row_selected(binding->batch, row))
		elog(ERROR, "pg_batch cannot select row %d", row);
	binding->consumer_ops->select_row(binding->slot, binding->batch, row);
}

static void
finish_batch(PgBatchBinding *binding)
{
	if (binding->batch == NULL || binding->batch_finished)
		elog(ERROR, "pg_batch cannot finish a slot without an active batch");
	binding->batch->generation = 0;
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
	if (binding->layout->target_columns != NULL)
		pfree((void *) binding->layout->target_columns);
	pfree(binding->layout);
	hash_search(slot_entries, &key, HASH_REMOVE, NULL);
	pfree(binding);
}

static void
get_row_view(PgBatchBinding *binding, PgBatchRowView *result)
{
	if (result == NULL || result->struct_size < PG_BATCH_ROW_VIEW_MIN_SIZE)
		elog(ERROR, "pg_batch received an incompatible row view");
	if (binding == NULL || binding->batch == NULL || binding->batch_finished)
		elog(ERROR, "pg_batch binding has no active batch");
	result->slot = binding->slot;
	result->batch = binding->batch;
	result->generation = binding->batch->generation;
	result->select_row = binding->consumer_ops->select_row;
}

void
_PG_init(void)
{
	void	  **rendezvous = find_rendezvous_variable(PG_BATCH_RENDEZVOUS);

	if (*rendezvous != NULL && *rendezvous != &bridge_api)
		elog(ERROR, "pg_batch bridge rendezvous variable is already in use");
	*rendezvous = (void *) &bridge_api;
}
