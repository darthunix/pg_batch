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
	void	   *consumer_state;
	const char *source_name;
	void	   *source_state;
	PgBatch *batch;
	const PgBatchOps *validated_ops;
	bool		request_sealed;
	bool		finished;
};

static HTAB *slot_entries;
static List *sources;
static List *nodes;
static uint64 next_batch_generation = 1;

static void register_source(const PgBatchSourceOps *source);
static void unregister_source(const char *source_name);
static const PgBatchSourceOps *find_source(Relation relation);
static const PgBatchSourceOps *get_source(const char *source_name);
static void register_node(const PgBatchNodeOps *node);
static void unregister_node(const char *node_name);
static const PgBatchNodeOps *find_node(const Path *path);
static const PgBatchNodeOps *get_node(const char *node_name);
static PgBatchBinding *find_binding(TupleTableSlot *slot);
static PgBatchBinding *attach_slot(TupleTableSlot *slot,
									 const PgBatchLayout *layout,
									 const PgBatchConsumerOps *consumer_ops,
									 void *consumer_state);
static void set_request(PgBatchBinding *binding,
						const PgBatchRequestSpec *spec);
static void set_source(PgBatchBinding *binding,
						 const char *source_name,
						 void *source_state);
static void *get_bound_source(PgBatchBinding *binding,
								const char **source_name);
static const PgBatchRequest *seal_request(PgBatchBinding *binding);
static const PgBatchRequest *get_request(PgBatchBinding *binding);
static void publish_batch(PgBatchBinding *binding,
						  PgBatch *batch);
static PgBatch *get_batch(PgBatchBinding *binding);
static void select_row(PgBatchBinding *binding, int row);
static void finish(PgBatchBinding *binding);
static bool finished(PgBatchBinding *binding);
static void clear(PgBatchBinding *binding);
static void detach_binding(PgBatchBinding *binding);
static void get_row_view(PgBatchBinding *binding, PgBatchRowView *result);

static const PgBatchAPI bridge_api = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_API_ABI_VERSION, PgBatchAPI),
	.register_source = register_source,
	.unregister_source = unregister_source,
	.find_source = find_source,
	.get_source = get_source,
	.register_node = register_node,
	.unregister_node = unregister_node,
	.find_node = find_node,
	.get_node = get_node,
	.find_binding = find_binding,
	.attach_slot = attach_slot,
	.set_request = set_request,
	.set_source = set_source,
	.get_bound_source = get_bound_source,
	.seal_request = seal_request,
	.get_request = get_request,
	.publish_batch = publish_batch,
	.get_batch = get_batch,
	.select_row = select_row,
	.finish = finish,
	.finished = finished,
	.clear = clear,
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
	binding->finished = false;
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
register_source(const PgBatchSourceOps *source)
{
	MemoryContext oldcontext;

	if (source == NULL)
		elog(ERROR, "pg_batch cannot register a null source");
	validate_abi(source->abi_version, source->struct_size,
				 PG_BATCH_SOURCE_OPS_ABI_VERSION,
				 "source", PG_BATCH_SOURCE_OPS_MIN_SIZE);
	if (source->source_name == NULL || source->source_name[0] == '\0')
		elog(ERROR, "pg_batch source must have a name");
	if (source->delivery != PG_BATCH_SOURCE_SLOT &&
		source->delivery != PG_BATCH_SOURCE_PULL)
		elog(ERROR, "pg_batch source \"%s\" has an invalid delivery mode",
			 source->source_name);
	if (source->supports_relation == NULL || source->plan_scan == NULL ||
		source->begin_scan == NULL || source->rescan == NULL ||
		source->end_scan == NULL)
		elog(ERROR, "pg_batch source \"%s\" has incomplete operations",
			 source->source_name);
	if (source->delivery == PG_BATCH_SOURCE_PULL &&
		(!PG_BATCH_ABI_HAS_FIELD(source, PgBatchSourceOps, next_batch) ||
		 source->next_batch == NULL))
		elog(ERROR, "pg_batch pull source \"%s\" has no next operation",
			 source->source_name);
	foreach_ptr(const PgBatchSourceOps, existing, sources)
	{
		if (strcmp(existing->source_name, source->source_name) == 0)
		{
			if (existing == source)
				return;
			elog(ERROR, "pg_batch source \"%s\" is already registered",
				 source->source_name);
		}
	}
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	sources = lappend(sources, (void *) source);
	MemoryContextSwitchTo(oldcontext);
}

static void
unregister_source(const char *source_name)
{
	ListCell   *lc;

	foreach(lc, sources)
	{
		const PgBatchSourceOps *source = lfirst(lc);

		if (strcmp(source->source_name, source_name) == 0)
		{
			sources = foreach_delete_current(sources, lc);
			return;
		}
	}
}

static const PgBatchSourceOps *
find_source(Relation relation)
{
	const PgBatchSourceOps *result = NULL;

	foreach_ptr(const PgBatchSourceOps, source, sources)
	{
		if (!source->supports_relation(relation))
			continue;
		if (result != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("multiple pg_batch sources support relation \"%s\"",
							RelationGetRelationName(relation)),
					 errdetail("Sources \"%s\" and \"%s\" both claimed the relation.",
							   result->source_name, source->source_name)));
		result = source;
	}
	return result;
}

static const PgBatchSourceOps *
get_source(const char *source_name)
{
	foreach_ptr(const PgBatchSourceOps, source, sources)
	{
		if (strcmp(source->source_name, source_name) == 0)
			return source;
	}
	return NULL;
}

static void
register_node(const PgBatchNodeOps *node)
{
	MemoryContext oldcontext;

	if (node == NULL)
		elog(ERROR, "pg_batch cannot register a null node");
	validate_abi(node->abi_version, node->struct_size,
				 PG_BATCH_NODE_OPS_ABI_VERSION,
				 "node", PG_BATCH_NODE_OPS_MIN_SIZE);
	if (node->node_name == NULL || node->node_name[0] == '\0' ||
		node->supports_path == NULL ||
		node->get_layout == NULL ||
		node->get_request_binding == NULL)
		elog(ERROR, "invalid pg_batch node registration");
	foreach_ptr(const PgBatchNodeOps, existing, nodes)
	{
		if (strcmp(existing->node_name, node->node_name) == 0)
		{
			if (existing == node)
				return;
			elog(ERROR, "pg_batch node \"%s\" is already registered",
				 node->node_name);
		}
	}
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	nodes = lappend(nodes, (void *) node);
	MemoryContextSwitchTo(oldcontext);
}

static void
unregister_node(const char *node_name)
{
	const PgBatchNodeOps *node = get_node(node_name);

	if (node != NULL)
		nodes = list_delete_ptr(nodes, (void *) node);
}

static const PgBatchNodeOps *
find_node(const Path *path)
{
	const PgBatchNodeOps *result = NULL;

	foreach_ptr(const PgBatchNodeOps, node, nodes)
	{
		if (!node->supports_path(path))
			continue;
		if (result != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("multiple pg_batch nodes support one plan path"),
					 errdetail("Nodes \"%s\" and \"%s\" both claimed the path.",
							   result->node_name, node->node_name)));
		result = node;
	}
	return result;
}

static const PgBatchNodeOps *
get_node(const char *node_name)
{
	foreach_ptr(const PgBatchNodeOps, node, nodes)
	{
		if (strcmp(node->node_name, node_name) == 0)
			return node;
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
			const PgBatchConsumerOps *consumer_ops,
			void *consumer_state)
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
	binding->request.spec.struct_size = sizeof(PgBatchRequestSpec);
	binding->consumer_ops = consumer_ops;
	binding->consumer_state = consumer_state;
	binding->cleanup.func = slot_reset;
	binding->cleanup.arg = binding;
	MemoryContextRegisterResetCallback(slot->tts_mcxt,
									   &binding->cleanup);
	return binding;
}

static void
set_request(PgBatchBinding *binding,
			const PgBatchRequestSpec *spec)
{
	MemoryContext oldcontext;
	Bitmapset  *new_filter;
	Bitmapset  *new_survivor;
	int			column;

	if (binding == NULL || spec == NULL ||
		spec->struct_size < PG_BATCH_REQUEST_SPEC_MIN_SIZE)
		elog(ERROR, "pg_batch received an invalid request specification");
	if (binding->request_sealed)
		elog(ERROR, "pg_batch request is already sealed");
	if (spec->max_rows < 0)
		elog(ERROR, "pg_batch maximum row count cannot be negative");
	if (spec->output_mode != PG_BATCH_OUTPUT_ROWS &&
		spec->output_mode != PG_BATCH_OUTPUT_BATCH)
		elog(ERROR, "pg_batch request has an invalid output mode");
	column = bms_next_member(spec->filter_columns,
							 binding->layout->ncolumns - 1);
	if (column >= 0)
		elog(ERROR, "pg_batch filter column %d is out of range", column);
	column = bms_next_member(spec->project_columns,
							 binding->layout->ncolumns - 1);
	if (column >= 0)
		elog(ERROR, "pg_batch project column %d is out of range", column);

	oldcontext = MemoryContextSwitchTo(binding->owner);
	new_filter = bms_copy(spec->filter_columns);
	new_survivor = bms_copy(spec->project_columns);
	MemoryContextSwitchTo(oldcontext);

	bms_free((Bitmapset *) binding->request.spec.filter_columns);
	bms_free((Bitmapset *) binding->request.spec.project_columns);
	binding->request.spec.filter_columns = new_filter;
	binding->request.spec.project_columns = new_survivor;
	binding->request.spec.output_mode = spec->output_mode;
	binding->request.spec.max_rows = spec->max_rows;
}

static void
set_source(PgBatchBinding *binding,
			 const char *source_name, void *source_state)
{
	if (binding == NULL)
		elog(ERROR, "pg_batch cannot configure a null binding");
	binding->source_name = source_name;
	binding->source_state = source_state;
}

static void *
get_bound_source(PgBatchBinding *binding, const char **source_name)
{
	if (source_name != NULL)
		*source_name = binding == NULL ? NULL : binding->source_name;
	return binding == NULL ? NULL : binding->source_state;
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
	if (batch->ops == NULL || batch->selection.words == NULL ||
		batch->selection.nrows <= 0 || batch->selection.nwords != (batch->selection.nrows + 63) / 64)
		elog(ERROR, "pg_batch source published an invalid batch");
	if (batch->generation != 0)
		elog(ERROR, "pg_batch source published a batch that is already active");
	if (binding->request.spec.max_rows > 0 &&
		batch->selection.nrows > binding->request.spec.max_rows)
		elog(ERROR, "pg_batch source exceeded the requested batch size");
	if (batch->selection.nrows % 64 != 0 &&
		(batch->selection.words[batch->selection.nwords - 1] &
		 (UINT64_MAX << (batch->selection.nrows % 64))) != 0)
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
	binding->finished = false;
	binding->consumer_ops->accept_batch(binding->consumer_state,
		binding->slot, batch);
}

static PgBatch *
get_batch(PgBatchBinding *binding)
{
	return binding == NULL ? NULL : binding->batch;
}

static void
select_row(PgBatchBinding *binding, int row)
{
	if (binding->batch == NULL || binding->finished ||
		row < 0 || row >= binding->batch->selection.nrows ||
		!pg_batch_selection_contains(&binding->batch->selection, row))
		elog(ERROR, "pg_batch cannot select row %d", row);
	binding->consumer_ops->select_row(binding->consumer_state, binding->slot,
		binding->batch, row);
}

static void
finish(PgBatchBinding *binding)
{
	if (binding->batch == NULL || binding->finished)
		elog(ERROR, "pg_batch cannot finish a slot without an active batch");
	binding->batch->generation = 0;
	binding->finished = true;
}

static bool
finished(PgBatchBinding *binding)
{
	if (binding == NULL)
		elog(ERROR, "pg_batch cannot inspect a null binding");
	return binding->batch == NULL || binding->finished;
}

static void
clear(PgBatchBinding *binding)
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
	bms_free((Bitmapset *) binding->request.spec.filter_columns);
	bms_free((Bitmapset *) binding->request.spec.project_columns);
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
	if (binding == NULL || binding->batch == NULL || binding->finished)
		elog(ERROR, "pg_batch binding has no active batch");
	result->slot = binding->slot;
	result->batch = binding->batch;
	result->generation = binding->batch->generation;
	result->consumer_state = binding->consumer_state;
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
