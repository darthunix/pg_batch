#include "postgres.h"

#include "fmgr.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "bridge.h"

PG_MODULE_MAGIC;

PGDLLEXPORT void _PG_init(void);

typedef struct SlotKey
{
	TupleTableSlot *slot;
} SlotKey;

typedef struct SlotEntry
{
	SlotKey		key;
	PgBatchBridgeBinding *binding;
} SlotEntry;

struct PgBatchBridgeBinding
{
	TupleTableSlot *slot;
	MemoryContext owner;
	MemoryContextCallback cleanup;
	PgBatchBridgeRequest request;
	const PgBatchBridgeConsumerOps *consumer_ops;
	PgBatchBridgeBatch *batch;
	const PgBatchBridgeBatchOps *validated_ops;
};

static HTAB *slot_entries;
static List *providers;

static void register_provider(const PgBatchBridgeProviderOps *provider);
static void unregister_provider(const char *provider_name);
static const PgBatchBridgeProviderOps *find_provider(Relation relation);
static const PgBatchBridgeProviderOps *get_provider(const char *provider_name);
static PgBatchBridgeBinding *find_binding(TupleTableSlot *slot);
static PgBatchBridgeBinding *attach_slot(TupleTableSlot *slot,
										 const AttrNumber *source_attnums,
										 int ncolumns,
										 const PgBatchBridgeConsumerOps *consumer_ops);
static void set_request(PgBatchBridgeBinding *binding,
						const Bitmapset *filter_columns,
						const Bitmapset *survivor_columns,
						bool return_batch);
static void set_provider(PgBatchBridgeBinding *binding,
						 const char *provider_name,
						 void *provider_state);
static const PgBatchBridgeRequest *get_request(PgBatchBridgeBinding *binding);
static void publish_batch(PgBatchBridgeBinding *binding,
						  PgBatchBridgeBatch *batch);
static PgBatchBridgeBatch *get_batch(PgBatchBridgeBinding *binding);
static void select_row(PgBatchBridgeBinding *binding, int row);
static void finish_batch(PgBatchBridgeBinding *binding);
static void clear_batch(PgBatchBridgeBinding *binding);
static void detach_binding(PgBatchBridgeBinding *binding);

static const PgBatchBridgeAPI bridge_api = {
	.abi_version = PG_BATCH_BRIDGE_ABI_VERSION,
	.struct_size = sizeof(PgBatchBridgeAPI),
	.register_provider = register_provider,
	.unregister_provider = unregister_provider,
	.find_provider = find_provider,
	.get_provider = get_provider,
	.find_binding = find_binding,
	.attach_slot = attach_slot,
	.set_request = set_request,
	.set_provider = set_provider,
	.get_request = get_request,
	.publish_batch = publish_batch,
	.get_batch = get_batch,
	.select_row = select_row,
	.finish_batch = finish_batch,
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
release_batch(PgBatchBridgeBinding *binding)
{
	PgBatchBridgeBatch *batch = binding->batch;

	if (batch == NULL)
		return;
	binding->batch = NULL;
	if (batch->ops->release != NULL)
		batch->ops->release(batch);
}

static void
slot_reset(void *arg)
{
	PgBatchBridgeBinding *binding = arg;
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
	if (version != PG_BATCH_BRIDGE_ABI_VERSION || struct_size < required_size)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("incompatible %s ABI", what),
				 errdetail("Expected version %u and at least %zu bytes, got version %u and %zu bytes.",
						   PG_BATCH_BRIDGE_ABI_VERSION, required_size,
						   version, struct_size)));
}

static void
register_provider(const PgBatchBridgeProviderOps *provider)
{
	MemoryContext oldcontext;

	if (provider == NULL || provider->provider_name == NULL ||
		provider->provider_name[0] == '\0')
		elog(ERROR, "pg_batch provider must have a name");
	validate_abi(provider->abi_version, provider->struct_size,
				 "provider", sizeof(PgBatchBridgeProviderOps));
	if (provider->supports_relation == NULL || provider->plan_scan == NULL ||
		provider->begin_scan == NULL || provider->rescan == NULL ||
		provider->end_scan == NULL)
		elog(ERROR, "pg_batch provider \"%s\" has incomplete operations",
			 provider->provider_name);
	foreach_ptr(const PgBatchBridgeProviderOps, existing, providers)
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
		const PgBatchBridgeProviderOps *provider = lfirst(lc);

		if (strcmp(provider->provider_name, provider_name) == 0)
		{
			providers = foreach_delete_current(providers, lc);
			return;
		}
	}
}

static const PgBatchBridgeProviderOps *
find_provider(Relation relation)
{
	const PgBatchBridgeProviderOps *result = NULL;

	foreach_ptr(const PgBatchBridgeProviderOps, provider, providers)
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

static const PgBatchBridgeProviderOps *
get_provider(const char *provider_name)
{
	foreach_ptr(const PgBatchBridgeProviderOps, provider, providers)
	{
		if (strcmp(provider->provider_name, provider_name) == 0)
			return provider;
	}
	return NULL;
}

static PgBatchBridgeBinding *
find_binding(TupleTableSlot *slot)
{
	SlotKey		key = {slot};
	SlotEntry  *entry;

	if (slot_entries == NULL)
		return NULL;
	entry = hash_search(slot_entries, &key, HASH_FIND, NULL);
	return entry == NULL ? NULL : entry->binding;
}

static PgBatchBridgeBinding *
attach_slot(TupleTableSlot *slot,
			const AttrNumber *source_attnums, int ncolumns,
			const PgBatchBridgeConsumerOps *consumer_ops)
{
	SlotKey		key = {slot};
	SlotEntry  *entry;
	PgBatchBridgeBinding *binding;
	AttrNumber *attnums = NULL;
	bool		found;

	if (slot == NULL || ncolumns < 0)
		elog(ERROR, "invalid pg_batch slot configuration");
	if (consumer_ops == NULL || consumer_ops->accept_batch == NULL ||
		consumer_ops->select_row == NULL)
		elog(ERROR, "pg_batch slot requires consumer operations");
	validate_abi(consumer_ops->abi_version,
				 consumer_ops->struct_size, "consumer",
				 sizeof(PgBatchBridgeConsumerOps));
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
set_request(PgBatchBridgeBinding *binding,
			const Bitmapset *filter_columns,
			const Bitmapset *survivor_columns,
			bool return_batch)
{
	MemoryContext oldcontext;
	Bitmapset  *new_filter;
	Bitmapset  *new_survivor;

	oldcontext = MemoryContextSwitchTo(binding->owner);
	new_filter = bms_copy(filter_columns);
	new_survivor = bms_copy(survivor_columns);
	MemoryContextSwitchTo(oldcontext);

	bms_free((Bitmapset *) binding->request.filter_columns);
	bms_free((Bitmapset *) binding->request.survivor_columns);
	binding->request.filter_columns = new_filter;
	binding->request.survivor_columns = new_survivor;
	binding->request.return_batch = return_batch;
}

static void
set_provider(PgBatchBridgeBinding *binding,
			 const char *provider_name, void *provider_state)
{
	binding->request.provider_name = provider_name;
	binding->request.provider_state = provider_state;
}

static const PgBatchBridgeRequest *
get_request(PgBatchBridgeBinding *binding)
{
	return binding == NULL ? NULL : &binding->request;
}

static void
publish_batch(PgBatchBridgeBinding *binding,
			  PgBatchBridgeBatch *batch)
{
	if (binding->batch != NULL)
		elog(ERROR, "pg_batch source published a new batch before clearing the previous one");
	if (batch == NULL || batch->ops == NULL || batch->selection == NULL ||
		batch->nrows <= 0 || batch->nwords != (batch->nrows + 63) / 64)
		elog(ERROR, "pg_batch source published an invalid batch");
	validate_abi(batch->abi_version, batch->struct_size,
				 "batch", sizeof(PgBatchBridgeBatch));
	if (batch->ops != binding->validated_ops)
	{
		validate_abi(batch->ops->abi_version,
					 batch->ops->struct_size, "batch operations",
					 sizeof(PgBatchBridgeBatchOps));
		if (batch->ops->get_datum_column == NULL)
			elog(ERROR, "pg_batch source must provide Datum materialization");
		binding->validated_ops = batch->ops;
	}
	batch->consumed = false;
	binding->batch = batch;
	binding->consumer_ops->accept_batch(binding->slot, batch);
}

static PgBatchBridgeBatch *
get_batch(PgBatchBridgeBinding *binding)
{
	return binding == NULL ? NULL : binding->batch;
}

static void
select_row(PgBatchBridgeBinding *binding, int row)
{
	if (binding->batch == NULL ||
		!pg_batch_bridge_row_selected(binding->batch, row))
		elog(ERROR, "pg_batch cannot select row %d", row);
	binding->consumer_ops->select_row(binding->slot, binding->batch, row);
}

static void
finish_batch(PgBatchBridgeBinding *binding)
{
	if (binding->batch == NULL)
		elog(ERROR, "pg_batch slot has no active batch");
	binding->batch->consumed = true;
}

static void
clear_batch(PgBatchBridgeBinding *binding)
{
	if (binding != NULL)
		release_batch(binding);
}

static void
detach_binding(PgBatchBridgeBinding *binding)
{
	SlotKey		key;

	if (binding == NULL)
		return;
	key.slot = binding->slot;
	release_batch(binding);
	MemoryContextUnregisterResetCallback(binding->owner,
										 &binding->cleanup);
	bms_free((Bitmapset *) binding->request.filter_columns);
	bms_free((Bitmapset *) binding->request.survivor_columns);
	if (binding->request.source_attnums != NULL)
		pfree((void *) binding->request.source_attnums);
	hash_search(slot_entries, &key, HASH_REMOVE, NULL);
	pfree(binding);
}

void
_PG_init(void)
{
	void	  **rendezvous = find_rendezvous_variable(PG_BATCH_BRIDGE_RENDEZVOUS);

	if (*rendezvous != NULL && *rendezvous != &bridge_api)
		elog(ERROR, "pg_batch bridge rendezvous variable is already in use");
	*rendezvous = (void *) &bridge_api;
}
