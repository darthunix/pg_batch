#include "postgres.h"

#include "runtime.h"

void
pg_batch_materialize_columns(PgBatchBridgeBatch *batch,
							 const Bitmapset *columns,
							 const uint64 *selected_rows,
							 PgBatchBridgeMaterializePhase phase)
{
	int			column = -1;

	while (columns != NULL &&
		   (column = bms_next_member(columns, column)) >= 0)
	{
		PgBatchBridgeDatumColumn ignored;

		pg_batch_get_datum_column(batch, column, selected_rows, phase,
							  &ignored);
	}
}

bool
pg_batch_get_arrow_column(PgBatchBridgeBatch *batch, int column,
						  PgBatchBridgeArrowView *result)
{
	const PgBatchBridgeArrowInterface *arrow;

	if (batch->ops->get_native_interface == NULL)
		return false;
	arrow = batch->ops->get_native_interface(
		batch, PG_BATCH_BRIDGE_ARROW_INTERFACE_NAME,
		PG_BATCH_BRIDGE_ARROW_INTERFACE_VERSION);
	if (arrow == NULL)
		return false;
	if (arrow->abi_version != PG_BATCH_BRIDGE_ARROW_INTERFACE_VERSION ||
		arrow->struct_size < sizeof(PgBatchBridgeArrowInterface))
		elog(ERROR, "pg_batch source returned an incompatible Arrow interface");
	arrow->get_column(batch, column, result);
	return true;
}

void
pg_batch_get_int4_vector(PgBatchBridgeBatch *batch, int column,
						 const uint64 *selected_rows,
						 PgBatchBridgeMaterializePhase phase,
						 PgBatchInt4Vector *result)
{
	PgBatchBridgeArrowView arrow;

	if (unlikely(batch->ops->get_native_interface != NULL) &&
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
		PgBatchBridgeDatumColumn datum;

		pg_batch_get_datum_column(batch, column, selected_rows, phase, &datum);
		pg_batch_int4_vector_init_datum(result, datum.values, datum.isnull,
								 datum.valid_rows, datum.nwords);
	}
}
