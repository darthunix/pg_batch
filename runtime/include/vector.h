/*
 * Borrowed views of batch columns shared by runtime consumers and kernels.
 *
 * A view describes a physical representation but does not own any referenced
 * memory. The source remains responsible for the arrays and their lifetime.
 */
#ifndef PG_BATCH_VECTOR_H
#define PG_BATCH_VECTOR_H

#include "postgres.h"

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
 * Datum values are indexed from zero. available marks rows that the source
 * has materialized. Packed values and validity are indexed from offset;
 * validity uses one for a non-NULL row and may be NULL when all rows are
 * valid.
 */
typedef struct PgBatchInt4Vector
{
	PgBatchInt4Layout layout;
	union
	{
		struct
		{
			const Datum *values;
			const bool *isnull;
			const uint64 *available;
			int			nwords;
		} datum;
		struct
		{
			const int32 *values;
			const uint8 *validity;
			int64		offset;
		} packed;
	} data;
} PgBatchInt4Vector;

/* Initialize a borrowed view over PostgreSQL Datum arrays. */
static inline void
pg_batch_int4_vector_init_datum(PgBatchInt4Vector *column,
								const Datum *values, const bool *isnull,
								const uint64 *available, int nwords)
{
	column->layout = PG_BATCH_INT4_DATUM;
	column->data.datum.values = values;
	column->data.datum.isnull = isnull;
	column->data.datum.available = available;
	column->data.datum.nwords = nwords;
}

/* Initialize a borrowed view over packed int32 values and validity bits. */
static inline void
pg_batch_int4_vector_init_packed(PgBatchInt4Vector *column,
								 const int32 *values, const uint8 *validity,
								 int64 offset)
{
	column->layout = PG_BATCH_INT4_PACKED;
	column->data.packed.values = values;
	column->data.packed.validity = validity;
	column->data.packed.offset = offset;
}

static inline bool
pg_batch_int4_row_available(const PgBatchInt4Vector *column, int row)
{
	if (column->layout == PG_BATCH_INT4_PACKED)
		return true;
	return row / 64 < column->data.datum.nwords &&
		(column->data.datum.available[row / 64] &
		 (UINT64CONST(1) << (row % 64))) != 0;
}

static inline bool
pg_batch_int4_row_is_null(const PgBatchInt4Vector *column, int row)
{
	Assert(pg_batch_int4_row_available(column, row));
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
	Assert(pg_batch_int4_row_available(column, row));
	if (column->layout == PG_BATCH_INT4_DATUM)
		return DatumGetInt32(column->data.datum.values[row]);
	return column->data.packed.values[column->data.packed.offset + row];
}

#endif							/* PG_BATCH_VECTOR_H */
