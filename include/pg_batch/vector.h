/*
 * Borrowed views of batch columns shared by runtime consumers and kernels.
 *
 * A view describes a physical representation but does not own any referenced
 * memory. The source remains responsible for the arrays and their lifetime.
 */
#ifndef PG_BATCH_VECTOR_H
#define PG_BATCH_VECTOR_H

#include "postgres.h"

#include "batch.h"

struct PgBatch;

typedef enum PgBatchInt4Layout
{
	/* PostgreSQL Datum values with one bool per NULL flag. */
	PG_BATCH_INT4_DATUM,
	/* Packed int32 values with an optional one-bit-per-row validity map. */
	PG_BATCH_INT4_PACKED
} PgBatchInt4Layout;

/*
 * Borrowed view of one int4 column.
 *
 * Datum values are indexed from zero. Packed values and validity are indexed
 * from offset. Validity uses one for a non-NULL row and may be NULL when all
 * rows are valid.
 */
typedef struct PgBatchInt4Vector
{
	Size		struct_size;
	PgBatchInt4Layout layout;
	union
	{
		struct
		{
			const Datum *values;
			const bool *isnull;
		} datum;
		struct
		{
			const int32 *values;
			const uint8 *validity;
			int64		offset;
		} packed;
	} data;
} PgBatchInt4Vector;

#define PG_BATCH_INT4_VECTOR_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchInt4Vector, data)

/*
 * Optional native batch interface for sources that already store int4
 * columns in one of the layouts above. Consumers discover it through
 * PgBatchOps.get_native_interface(). Returning false for a column
 * asks the runtime to fall back to another native interface or to Datums.
 */
#define PG_BATCH_INT4_VECTOR_INTERFACE_NAME "pg_batch.int4_vector.v1"
#define PG_BATCH_INT4_VECTOR_INTERFACE_VERSION 1

extern const PgBatchNativeType pg_batch_int4_vector_type;

typedef struct PgBatchInt4VectorInterface
{
	uint32		abi_version;
	Size		struct_size;
	bool		(*get_column) (struct PgBatch *batch, int column,
							 PgBatchInt4Vector *result);
} PgBatchInt4VectorInterface;

#define PG_BATCH_INT4_VECTOR_INTERFACE_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchInt4VectorInterface, get_column)

/* Initialize a borrowed view over PostgreSQL Datum arrays. */
static inline void
pg_batch_int4_vector_init_datum(PgBatchInt4Vector *column,
								const Datum *values, const bool *isnull)
{
	column->struct_size = sizeof(*column);
	column->layout = PG_BATCH_INT4_DATUM;
	column->data.datum.values = values;
	column->data.datum.isnull = isnull;
}

/* Initialize a borrowed view over packed int32 values and validity bits. */
static inline void
pg_batch_int4_vector_init_packed(PgBatchInt4Vector *column,
								 const int32 *values, const uint8 *validity,
								 int64 offset)
{
	column->struct_size = sizeof(*column);
	column->layout = PG_BATCH_INT4_PACKED;
	column->data.packed.values = values;
	column->data.packed.validity = validity;
	column->data.packed.offset = offset;
}

static inline bool
pg_batch_int4_row_is_null(const PgBatchInt4Vector *column, int row)
{
	if (column->layout == PG_BATCH_INT4_DATUM)
		return column->data.datum.isnull[row];
	if (column->data.packed.validity == NULL)
		return false;
	row += column->data.packed.offset;
	return (column->data.packed.validity[row / 8] &
			((uint8) 1 << (row % 8))) == 0;
}

static inline int32
pg_batch_int4_row_value(const PgBatchInt4Vector *column, int row)
{
	if (column->layout == PG_BATCH_INT4_DATUM)
		return DatumGetInt32(column->data.datum.values[row]);
	return column->data.packed.values[column->data.packed.offset + row];
}

#endif							/* PG_BATCH_VECTOR_H */
