#include "postgres.h"

#include "utils/memutils.h"

#include "runtime.h"

const PgBatchNativeType pg_batch_int4_vector_type = {
	.name = PG_BATCH_INT4_VECTOR_INTERFACE_NAME,
	.abi_version = PG_BATCH_INT4_VECTOR_INTERFACE_VERSION,
	.min_size = PG_BATCH_INT4_VECTOR_INTERFACE_MIN_SIZE,
};

const PgBatchNativeType pg_batch_arrow_type = {
	.name = PG_BATCH_ARROW_INTERFACE_NAME,
	.abi_version = PG_BATCH_ARROW_INTERFACE_VERSION,
	.min_size = PG_BATCH_ARROW_INTERFACE_MIN_SIZE,
};

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
							 const uint64 *selected_rows,
							 PgBatchColumnPhase phase)
{
	int			column = -1;

	while (columns != NULL &&
		   (column = bms_next_member(columns, column)) >= 0)
	{
		PgBatchDatumVector ignored;

		pg_batch_get_datum_column(batch, column, selected_rows, phase,
							  &ignored);
	}
}

bool
pg_batch_get_arrow_column(PgBatch *batch, int column,
						  PgBatchArrowView *result)
{
	const PgBatchArrowInterface *arrow;

	arrow = (const PgBatchArrowInterface *) pg_batch_get_native_interface(
		batch, &pg_batch_arrow_type);
	if (arrow == NULL)
		return false;
	*result = (PgBatchArrowView) PG_BATCH_STRUCT_INITIALIZER(PgBatchArrowView);
	arrow->get_column(batch, column, result);
	if (result->struct_size < PG_BATCH_ARROW_VIEW_MIN_SIZE ||
		result->array == NULL || result->schema == NULL)
		elog(ERROR, "pg_batch source returned an incompatible Arrow view");
	return true;
}

void
pg_batch_get_int4_vector(PgBatch *batch, int column,
						 const uint64 *selected_rows,
						 PgBatchColumnPhase phase,
						 PgBatchInt4Vector *result)
{
	const PgBatchInt4VectorInterface *native = NULL;
	PgBatchArrowView arrow;

	native = (const PgBatchInt4VectorInterface *)
		pg_batch_get_native_interface(batch, &pg_batch_int4_vector_type);
	if (native != NULL)
	{
		*result = (PgBatchInt4Vector) {0};
		result->struct_size = sizeof(*result);
		if (native->get_column(batch, column, result))
		{
			if (result->struct_size < PG_BATCH_INT4_VECTOR_MIN_SIZE)
				elog(ERROR, "pg_batch source returned an incompatible int4 vector");
			return;
		}
	}

	if (pg_batch_get_arrow_column(batch, column, &arrow))
	{
		const struct ArrowArray *array = arrow.array;

		if (strcmp(arrow.schema->format, "i") != 0)
			elog(ERROR, "pg_batch int4 kernel received a non-int4 Arrow column");
		pg_batch_int4_vector_init_packed(result, array->buffers[1],
			array->null_count == 0 ? NULL : array->buffers[0], array->offset);
	}
	else
	{
		PgBatchDatumVector datum;

		pg_batch_get_datum_column(batch, column, selected_rows, phase, &datum);
		pg_batch_int4_vector_init_datum(result, datum.values, datum.isnull,
								 datum.valid_rows, datum.nwords);
	}
}
