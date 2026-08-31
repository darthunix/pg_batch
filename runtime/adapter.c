#include "postgres.h"

#include "runtime.h"

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

	if (!PG_BATCH_ABI_HAS_FIELD(batch->ops, PgBatchOps,
			get_native_interface) || batch->ops->get_native_interface == NULL)
		return false;
	arrow = batch->ops->get_native_interface(
		batch, PG_BATCH_ARROW_INTERFACE_NAME,
		PG_BATCH_ARROW_INTERFACE_VERSION);
	if (arrow == NULL)
		return false;
	if (arrow->abi_version != PG_BATCH_ARROW_INTERFACE_VERSION ||
		arrow->struct_size < PG_BATCH_ARROW_INTERFACE_MIN_SIZE)
		elog(ERROR, "pg_batch source returned an incompatible Arrow interface");
	arrow->get_column(batch, column, result);
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

	if (unlikely(PG_BATCH_ABI_HAS_FIELD(batch->ops, PgBatchOps,
			get_native_interface) &&
			batch->ops->get_native_interface != NULL))
		native = batch->ops->get_native_interface(
			batch, PG_BATCH_INT4_VECTOR_INTERFACE_NAME,
			PG_BATCH_INT4_VECTOR_INTERFACE_VERSION);
	if (native != NULL)
	{
		if (native->abi_version != PG_BATCH_INT4_VECTOR_INTERFACE_VERSION ||
			native->struct_size < PG_BATCH_INT4_VECTOR_INTERFACE_MIN_SIZE)
			elog(ERROR, "pg_batch source returned an incompatible int4 vector interface");
		if (native->get_column(batch, column, result))
			return;
	}

	if (unlikely(PG_BATCH_ABI_HAS_FIELD(batch->ops, PgBatchOps,
			get_native_interface) &&
			batch->ops->get_native_interface != NULL) &&
		pg_batch_get_arrow_column(batch, column, &arrow))
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
