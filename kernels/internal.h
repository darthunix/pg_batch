#ifndef PG_BATCH_KERNELS_INTERNAL_H
#define PG_BATCH_KERNELS_INTERNAL_H

#include "kernels.h"

static inline bool
pg_batch_int4_compare(int32 left, int32 right, PgBatchInt4Op op)
{
	switch (op)
	{
		case PG_BATCH_INT4_EQ:
			return left == right;
		case PG_BATCH_INT4_NE:
			return left != right;
		case PG_BATCH_INT4_LT:
			return left < right;
		case PG_BATCH_INT4_LE:
			return left <= right;
		case PG_BATCH_INT4_GT:
			return left > right;
		case PG_BATCH_INT4_GE:
			return left >= right;
	}
	pg_unreachable();
}

extern uint64 pg_batch_int4_simd_compare(const PgBatchInt4Vector *column,
										 int first_row, int nrows,
										 PgBatchInt4Op op, int32 scalar);

#endif							/* PG_BATCH_KERNELS_INTERNAL_H */
