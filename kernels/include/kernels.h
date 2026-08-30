/*
 * Type-specific operations over batch columns.
 *
 * A kernel consumes a borrowed column view and updates the caller-owned row
 * selection. Physical sources keep ownership of all referenced arrays.
 */
#ifndef PG_BATCH_KERNELS_H
#define PG_BATCH_KERNELS_H

#include "postgres.h"

#include "vector.h"

typedef enum PgBatchInt4Op
{
	PG_BATCH_INT4_EQ,
	PG_BATCH_INT4_NE,
	PG_BATCH_INT4_LT,
	PG_BATCH_INT4_LE,
	PG_BATCH_INT4_GT,
	PG_BATCH_INT4_GE
} PgBatchInt4Op;

/* Arithmetic supported by the int4 expression executor. */
typedef enum PgBatchInt4ArithmeticOp
{
	PG_BATCH_INT4_ADD,
	PG_BATCH_INT4_SUBTRACT,
	PG_BATCH_INT4_MULTIPLY,
	PG_BATCH_INT4_DIVIDE,
	PG_BATCH_INT4_MODULO,
	PG_BATCH_INT4_NEGATE
} PgBatchInt4ArithmeticOp;

/* Values that one int4 reduction pass should calculate. */
typedef enum PgBatchInt4ReduceFlag
{
	PG_BATCH_INT4_REDUCE_COUNT = 1 << 0,
	PG_BATCH_INT4_REDUCE_SUM = 1 << 1,
	PG_BATCH_INT4_REDUCE_MIN = 1 << 2,
	PG_BATCH_INT4_REDUCE_MAX = 1 << 3
} PgBatchInt4ReduceFlag;

/* Partial result for one column and one batch. */
typedef struct PgBatchInt4Reduction
{
	int64		count;
	int64		sum;
	int32		min;
	int32		max;
	bool		has_value;
} PgBatchInt4Reduction;

/* Map a built-in int4 comparison and normalize a Var on the right. */
extern bool pg_batch_int4_compare_op(Oid funcid, uint8 var_argno,
									 PgBatchInt4Op *result);

/* Remove rows that are NULL or do not satisfy column op scalar. */
extern void pg_batch_filter_int4(const PgBatchInt4Vector *column,
								 int nrows, int nwords, uint64 *selection,
								 PgBatchInt4Op op, int32 scalar,
								 bool enable_simd);

/*
 * Apply one arithmetic operation to selected, non-NULL rows. Output values
 * are packed int32 values indexed by row. output_validity is an Arrow-style
 * bitmap: one means that the output value is present and non-NULL.
 */
extern void pg_batch_int4_arithmetic_scalar(
	const PgBatchInt4Vector *input, int nrows, int nwords,
	const uint64 *selection, PgBatchInt4ArithmeticOp op, int32 scalar,
	bool scalar_on_left, int32 *output_values, uint8 *output_validity);

/* Calculate the requested reductions without changing the row selection. */
extern void pg_batch_reduce_int4(const PgBatchInt4Vector *column,
								 int nrows, int nwords,
								 const uint64 *selection, uint32 flags,
								 PgBatchInt4Reduction *result);

/* Hash selected rows and reject rows with a NULL in any key column. */
extern void pg_batch_hash_int4(const PgBatchInt4Vector *const *keys,
							   int nkeys, int nrows, int nwords,
							   const uint64 *selection, uint32 *hashes,
							   uint64 *valid_rows);

extern bool pg_batch_int4_simd_available(void);

#endif							/* PG_BATCH_KERNELS_H */
