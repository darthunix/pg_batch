#include "postgres.h"

#include "utils/memutils.h"

#include "runtime.h"

typedef struct NativeCacheEntry
{
	const PgBatchOps *ops;
	const char *name;
	uint32		abi_version;
	const PgBatchNativeInterface *interface;
} NativeCacheEntry;

static List *native_interfaces;
static NativeCacheEntry *last_native_interface;

static const PgBatchNativeInterface *
checked_native_interface(const NativeCacheEntry *entry,
						 const PgBatchNativeType *type)
{
	if (entry->interface != NULL &&
		entry->interface->struct_size < type->min_size)
		elog(ERROR, "pg_batch source returned an incompatible native interface \"%s\"",
			 type->name);
	return entry->interface;
}

const PgBatchNativeInterface *
pg_batch_get_native_interface(PgBatch *batch, const PgBatchNativeType *type)
{
	const PgBatchNativeInterface *result;
	NativeCacheEntry *entry;
	MemoryContext oldcontext;

	if (batch == NULL || type == NULL || type->name == NULL)
		elog(ERROR, "pg_batch received an invalid native interface request");
	if (!PG_BATCH_ABI_HAS_FIELD(batch->ops, PgBatchOps,
			get_native_interface) || batch->ops->get_native_interface == NULL)
		return NULL;
	if (last_native_interface != NULL &&
		last_native_interface->ops == batch->ops &&
		last_native_interface->abi_version == type->abi_version &&
		strcmp(last_native_interface->name, type->name) == 0)
		return checked_native_interface(last_native_interface, type);
	foreach_ptr(NativeCacheEntry, cached, native_interfaces)
	{
		if (cached->ops == batch->ops &&
			cached->abi_version == type->abi_version &&
			strcmp(cached->name, type->name) == 0)
		{
			last_native_interface = cached;
			return checked_native_interface(cached, type);
		}
	}

	result = batch->ops->get_native_interface(type);
	if (result != NULL &&
		(result->abi_version != type->abi_version ||
		 result->struct_size < type->min_size))
		elog(ERROR, "pg_batch source returned an incompatible native interface \"%s\"",
			 type->name);
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	entry = palloc(sizeof(*entry));
	entry->ops = batch->ops;
	entry->name = pstrdup(type->name);
	entry->abi_version = type->abi_version;
	entry->interface = result;
	native_interfaces = lappend(native_interfaces, entry);
	MemoryContextSwitchTo(oldcontext);
	last_native_interface = entry;
	return result;
}

void
pg_batch_materialize_columns(PgBatch *batch,
							 const Bitmapset *columns,
							 const PgBatchSelection *selected_rows,
							 PgBatchColumnPhase phase)
{
	PgBatchColumnAccess access;
	int			column = -1;

	pg_batch_column_access_init(&access, batch, columns, selected_rows, phase);
	while (columns != NULL &&
		   (column = bms_next_member(columns, column)) >= 0)
	{
		PgBatchDatumVector ignored;

		pg_batch_column_get_datum(&access, column, &ignored);
	}
}
