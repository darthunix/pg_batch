#include "postgres.h"

#include "kernels.h"
#include "runtime.h"

const PgBatchNativeType pg_batch_int4_vector_type = {
	.name = PG_BATCH_INT4_VECTOR_INTERFACE_NAME,
	.abi_version = PG_BATCH_INT4_VECTOR_INTERFACE_VERSION,
	.min_size = PG_BATCH_INT4_VECTOR_INTERFACE_MIN_SIZE,
};

void
pg_batch_column_get_int4(PgBatchColumnAccess *access, int column,
						 PgBatchInt4Vector *result)
{
	const PgBatchInt4VectorInterface *native;

	if (access == NULL || access->batch == NULL ||
		!bms_is_member(column, access->columns))
		elog(ERROR, "pg_batch column %d was not requested", column + 1);
	native = (const PgBatchInt4VectorInterface *)
		pg_batch_get_native_interface(access->batch,
			&pg_batch_int4_vector_type);
	if (native != NULL)
	{
		*result = (PgBatchInt4Vector) {0};
		result->struct_size = sizeof(*result);
		if (native->get_column(access->batch, column, result))
		{
			if (result->struct_size < PG_BATCH_INT4_VECTOR_MIN_SIZE)
				elog(ERROR, "pg_batch source returned an incompatible int4 vector");
			return;
		}
	}

	{
		PgBatchDatumVector datum;

		pg_batch_column_get_datum(access, column, &datum);
		pg_batch_int4_vector_init_datum(result, datum.values, datum.isnull);
	}
}
