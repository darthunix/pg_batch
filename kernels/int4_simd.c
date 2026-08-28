#include "postgres.h"

#include "internal.h"

#if defined(USE_NEON)
#include <arm_neon.h>

typedef int32x4_t Int4Vector;
typedef uint32x4_t Int4Mask;
#elif defined(USE_SSE2)
#include <emmintrin.h>
#include <xmmintrin.h>

typedef __m128i Int4Vector;
typedef __m128i Int4Mask;
#endif

bool
pg_batch_int4_simd_available(void)
{
#if defined(USE_NEON) || defined(USE_SSE2)
	return true;
#else
	return false;
#endif
}

#if defined(USE_NEON)
static inline Int4Vector
load_datum_values(const Datum *values)
{
	int64x2_t	first = vreinterpretq_s64_u64(vld1q_u64(values));
	int64x2_t	second = vreinterpretq_s64_u64(vld1q_u64(values + 2));

	return vcombine_s32(vmovn_s64(first), vmovn_s64(second));
}

static inline Int4Vector
load_packed_values(const int32 *values)
{
	return vld1q_s32(values);
}

static inline uint32
comparison_mask(Int4Mask comparison)
{
	uint32		lanes[4];

	vst1q_u32(lanes, comparison);
	return ((lanes[0] >> 31) |
			((lanes[1] >> 31) << 1) |
			((lanes[2] >> 31) << 2) |
			((lanes[3] >> 31) << 3));
}
#elif defined(USE_SSE2)
static inline Int4Vector
load_datum_values(const Datum *values)
{
	__m128i		first = _mm_loadu_si128((const __m128i *) values);
	__m128i		second = _mm_loadu_si128((const __m128i *) (values + 2));

	first = _mm_shuffle_epi32(first, _MM_SHUFFLE(2, 0, 2, 0));
	second = _mm_shuffle_epi32(second, _MM_SHUFFLE(2, 0, 2, 0));
	return _mm_unpacklo_epi64(first, second);
}

static inline Int4Vector
load_packed_values(const int32 *values)
{
	return _mm_loadu_si128((const __m128i *) values);
}

static inline uint32
comparison_mask(Int4Mask comparison)
{
	return _mm_movemask_ps(_mm_castsi128_ps(comparison));
}
#endif

uint64
pg_batch_int4_simd_compare(const PgBatchInt4Vector *column, int first_row,
						   int nrows, PgBatchInt4Op op, int32 scalar)
{
#if defined(USE_NEON) || defined(USE_SSE2)
	uint64		result = 0;
	int			row = 0;
#if defined(USE_NEON)
	Int4Vector right = vdupq_n_s32(scalar);
#define INT4_EQ(left, right) vceqq_s32((left), (right))
#define INT4_NE(left, right) vmvnq_u32(vceqq_s32((left), (right)))
#define INT4_LT(left, right) vcltq_s32((left), (right))
#define INT4_LE(left, right) vcleq_s32((left), (right))
#define INT4_GT(left, right) vcgtq_s32((left), (right))
#define INT4_GE(left, right) vcgeq_s32((left), (right))
#else
	Int4Vector right = _mm_set1_epi32(scalar);
#define INT4_ALL(left) _mm_cmpeq_epi32((left), (left))
#define INT4_EQ(left, right) _mm_cmpeq_epi32((left), (right))
#define INT4_NE(left, right) \
	_mm_xor_si128(_mm_cmpeq_epi32((left), (right)), INT4_ALL(left))
#define INT4_LT(left, right) _mm_cmpgt_epi32((right), (left))
#define INT4_LE(left, right) \
	_mm_xor_si128(_mm_cmpgt_epi32((left), (right)), INT4_ALL(left))
#define INT4_GT(left, right) _mm_cmpgt_epi32((left), (right))
#define INT4_GE(left, right) \
	_mm_xor_si128(_mm_cmpgt_epi32((right), (left)), INT4_ALL(left))
#endif

#define RUN_COMPARISON(expression) \
	do { \
		for (; row + 4 <= nrows; row += 4) \
		{ \
			Int4Vector left; \
			Int4Mask comparison; \
			if (column->layout == PG_BATCH_INT4_DATUM) \
				left = load_datum_values(column->data.datum.values + \
									 first_row + row); \
			else \
				left = load_packed_values(column->data.packed.values + \
									 column->data.packed.offset + \
									 first_row + row); \
			comparison = (expression); \
			result |= (uint64) comparison_mask(comparison) << row; \
		} \
	} while (0)

	switch (op)
	{
		case PG_BATCH_INT4_EQ:
			RUN_COMPARISON(INT4_EQ(left, right));
			break;
		case PG_BATCH_INT4_NE:
			RUN_COMPARISON(INT4_NE(left, right));
			break;
		case PG_BATCH_INT4_LT:
			RUN_COMPARISON(INT4_LT(left, right));
			break;
		case PG_BATCH_INT4_LE:
			RUN_COMPARISON(INT4_LE(left, right));
			break;
		case PG_BATCH_INT4_GT:
			RUN_COMPARISON(INT4_GT(left, right));
			break;
		case PG_BATCH_INT4_GE:
			RUN_COMPARISON(INT4_GE(left, right));
			break;
	}
#undef RUN_COMPARISON
#undef INT4_EQ
#undef INT4_NE
#undef INT4_LT
#undef INT4_LE
#undef INT4_GT
#undef INT4_GE
#ifdef INT4_ALL
#undef INT4_ALL
#endif
	for (; row < nrows; row++)
	{
		int			physical_row = first_row + row;
		int32		value;

		if (column->layout == PG_BATCH_INT4_DATUM)
			value = DatumGetInt32(column->data.datum.values[physical_row]);
		else
			value = column->data.packed.values[
				column->data.packed.offset + physical_row];
		if (pg_batch_int4_compare(value, scalar, op))
			result |= UINT64CONST(1) << row;
	}
	return result;
#else
	return 0;
#endif
}
