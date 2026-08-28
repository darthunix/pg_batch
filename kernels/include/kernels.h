/*
 * Type-specific operations over batch columns.
 *
 * A kernel consumes a borrowed column view and updates the caller-owned row
 * selection. Physical sources keep ownership of all referenced arrays.
 */
#ifndef PG_BATCH_KERNELS_H
#define PG_BATCH_KERNELS_H

#include "postgres.h"

typedef enum PgBatchInt4Op
{
	PG_BATCH_INT4_EQ,
	PG_BATCH_INT4_NE,
	PG_BATCH_INT4_LT,
	PG_BATCH_INT4_LE,
	PG_BATCH_INT4_GT,
	PG_BATCH_INT4_GE
} PgBatchInt4Op;

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

/* Map a built-in int4 comparison and normalize a Var on the right. */
extern bool pg_batch_int4_compare_op(Oid funcid, uint8 var_argno,
									 PgBatchInt4Op *result);

/* Remove rows that are NULL or do not satisfy column op scalar. */
extern void pg_batch_filter_int4(const PgBatchInt4Vector *column,
								 int nrows, int nwords, uint64 *selection,
								 PgBatchInt4Op op, int32 scalar,
								 bool enable_simd);

extern bool pg_batch_int4_simd_available(void);

#endif							/* PG_BATCH_KERNELS_H */
