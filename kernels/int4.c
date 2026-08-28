#include "postgres.h"

#include "port/pg_bitutils.h"
#include "utils/fmgroids.h"

#include "internal.h"

bool
pg_batch_int4_compare_op(Oid funcid, uint8 var_argno,
						 PgBatchInt4Op *result)
{
	PgBatchInt4Op op;

	switch (funcid)
	{
		case F_INT4EQ:
			op = PG_BATCH_INT4_EQ;
			break;
		case F_INT4NE:
			op = PG_BATCH_INT4_NE;
			break;
		case F_INT4LT:
			op = PG_BATCH_INT4_LT;
			break;
		case F_INT4LE:
			op = PG_BATCH_INT4_LE;
			break;
		case F_INT4GT:
			op = PG_BATCH_INT4_GT;
			break;
		case F_INT4GE:
			op = PG_BATCH_INT4_GE;
			break;
		default:
			return false;
	}

	if (var_argno == 1)
	{
		if (op == PG_BATCH_INT4_LT)
			op = PG_BATCH_INT4_GT;
		else if (op == PG_BATCH_INT4_LE)
			op = PG_BATCH_INT4_GE;
		else if (op == PG_BATCH_INT4_GT)
			op = PG_BATCH_INT4_LT;
		else if (op == PG_BATCH_INT4_GE)
			op = PG_BATCH_INT4_LE;
	}
	*result = op;
	return true;
}

static inline uint64
word_mask(int nrows, int word)
{
	int			remaining = nrows - word * 64;

	if (remaining >= 64)
		return UINT64_MAX;
	Assert(remaining > 0);
	return UINT64_MAX >> (64 - remaining);
}

static uint64
scalar_compare(const PgBatchInt4Vector *column, int first_row, uint64 rows,
			   PgBatchInt4Op op, int32 scalar)
{
	uint64		result = 0;

	/* Dispatch once per word, not once per selected row. */
#define RUN_SCALAR(expression) \
	do { \
		while (rows != 0) \
		{ \
			int bitno = pg_rightmost_one_pos64(rows); \
			uint64 bit = UINT64CONST(1) << bitno; \
			int row = first_row + bitno; \
			int32 value; \
			if (!pg_batch_int4_row_is_null(column, row)) \
			{ \
				value = pg_batch_int4_row_value(column, row); \
				if (expression) \
					result |= bit; \
			} \
			rows &= ~bit; \
		} \
	} while (0)

	switch (op)
	{
		case PG_BATCH_INT4_EQ:
			RUN_SCALAR(value == scalar);
			break;
		case PG_BATCH_INT4_NE:
			RUN_SCALAR(value != scalar);
			break;
		case PG_BATCH_INT4_LT:
			RUN_SCALAR(value < scalar);
			break;
		case PG_BATCH_INT4_LE:
			RUN_SCALAR(value <= scalar);
			break;
		case PG_BATCH_INT4_GT:
			RUN_SCALAR(value > scalar);
			break;
		case PG_BATCH_INT4_GE:
			RUN_SCALAR(value >= scalar);
			break;
	}
#undef RUN_SCALAR
	return result;
}

static uint64
not_null_mask(const PgBatchInt4Vector *column, int first_row, int nrows,
			  uint64 rows)
{
	uint64		result;

	if (column->layout == PG_BATCH_INT4_PACKED &&
		column->data.packed.validity == NULL)
		return rows;
	if (column->layout == PG_BATCH_INT4_DATUM &&
		rows == word_mask(nrows, 0) &&
		memchr(column->data.datum.isnull + first_row, true, nrows) == NULL)
		return rows;

	result = 0;
	while (rows != 0)
	{
		int			bitno = pg_rightmost_one_pos64(rows);
		uint64		bit = UINT64CONST(1) << bitno;

		if (!pg_batch_int4_row_is_null(column, first_row + bitno))
			result |= bit;
		rows &= ~bit;
	}
	return result;
}

void
pg_batch_filter_int4(const PgBatchInt4Vector *column, int nrows, int nwords,
					 uint64 *selection, PgBatchInt4Op op, int32 scalar,
					 bool enable_simd)
{
	bool		use_simd = enable_simd && pg_batch_int4_simd_available();

	Assert(nrows > 0);
	Assert(nwords == (nrows + 63) / 64);

	for (int word = 0; word < nwords; word++)
	{
		uint64		rows = selection[word] & word_mask(nrows, word);
		int			first_row = word * 64;
		int			word_nrows = Min(64, nrows - first_row);

		if (rows == 0)
			continue;
		if (column->layout == PG_BATCH_INT4_DATUM &&
			(word >= column->data.datum.nwords ||
			 (column->data.datum.available[word] & rows) != rows))
			elog(ERROR, "pg_batch int4 kernel received unavailable Datum rows");

		/* Sparse selections are cheaper to visit through their set bits. */
		if (use_simd &&
			rows == word_mask(nrows, word) && word_nrows >= 4)
		{
			uint64		passing;

			passing = pg_batch_int4_simd_compare(column, first_row,
											 word_nrows, op, scalar);
			passing &= not_null_mask(column, first_row, word_nrows, rows);
			selection[word] = rows & passing;
		}
		else
			selection[word] = scalar_compare(column, first_row, rows,
										 op, scalar);
	}
}

static inline void
reduce_value(PgBatchInt4Reduction *result, uint32 flags, int32 value)
{
	if (flags & PG_BATCH_INT4_REDUCE_SUM)
		result->sum += value;
	if (!result->has_value)
	{
		if (flags & PG_BATCH_INT4_REDUCE_MIN)
			result->min = value;
		if (flags & PG_BATCH_INT4_REDUCE_MAX)
			result->max = value;
		result->has_value = true;
	}
	else
	{
		if ((flags & PG_BATCH_INT4_REDUCE_MIN) && value < result->min)
			result->min = value;
		if ((flags & PG_BATCH_INT4_REDUCE_MAX) && value > result->max)
			result->max = value;
	}
}

static int64
reduce_sum(const PgBatchInt4Vector *column, int first_row, int nrows,
		   uint64 rows)
{
	int64		sum = 0;

	if (rows == word_mask(nrows, 0))
	{
		if (column->layout == PG_BATCH_INT4_DATUM)
		{
			const Datum *values = column->data.datum.values + first_row;

			for (int row = 0; row < nrows; row++)
				sum += DatumGetInt32(values[row]);
		}
		else
		{
			const int32 *values = column->data.packed.values +
				column->data.packed.offset + first_row;

			for (int row = 0; row < nrows; row++)
				sum += values[row];
		}
		return sum;
	}

	if (column->layout == PG_BATCH_INT4_DATUM)
	{
		const Datum *values = column->data.datum.values + first_row;

		while (rows != 0)
		{
			int			bitno = pg_rightmost_one_pos64(rows);

			sum += DatumGetInt32(values[bitno]);
			rows &= rows - 1;
		}
	}
	else
	{
		const int32 *values = column->data.packed.values +
			column->data.packed.offset + first_row;

		while (rows != 0)
		{
			int			bitno = pg_rightmost_one_pos64(rows);

			sum += values[bitno];
			rows &= rows - 1;
		}
	}
	return sum;
}

void
pg_batch_reduce_int4(const PgBatchInt4Vector *column, int nrows, int nwords,
					 const uint64 *selection, uint32 flags,
					 PgBatchInt4Reduction *result)
{
	uint32		value_flags = flags & (PG_BATCH_INT4_REDUCE_SUM |
									 PG_BATCH_INT4_REDUCE_MIN |
									 PG_BATCH_INT4_REDUCE_MAX);

	Assert(nrows > 0);
	Assert(nwords == (nrows + 63) / 64);
	Assert(flags != 0);
	MemSet(result, 0, sizeof(*result));

	for (int word = 0; word < nwords; word++)
	{
		int			first_row = word * 64;
		int			word_nrows = Min(64, nrows - first_row);
		uint64		rows = selection[word] & word_mask(nrows, word);

		if (rows == 0)
			continue;
		if (column->layout == PG_BATCH_INT4_DATUM &&
			(word >= column->data.datum.nwords ||
			 (column->data.datum.available[word] & rows) != rows))
			elog(ERROR, "pg_batch int4 kernel received unavailable Datum rows");

		rows &= not_null_mask(column, first_row, word_nrows, rows);
		if (rows == 0)
			continue;
		if (flags & PG_BATCH_INT4_REDUCE_COUNT)
			result->count += pg_popcount64(rows);
		if (value_flags == 0)
		{
			result->has_value = true;
			continue;
		}
		if (value_flags == PG_BATCH_INT4_REDUCE_SUM)
		{
			result->sum += reduce_sum(column, first_row, word_nrows, rows);
			result->has_value = true;
			continue;
		}

		if (rows == word_mask(nrows, word))
		{
			for (int row = first_row; row < first_row + word_nrows; row++)
				reduce_value(result, value_flags,
							 pg_batch_int4_row_value(column, row));
		}
		else
		{
			while (rows != 0)
			{
				int			bitno = pg_rightmost_one_pos64(rows);
				uint64		bit = UINT64CONST(1) << bitno;
				int			row = first_row + bitno;

				reduce_value(result, value_flags,
							 pg_batch_int4_row_value(column, row));
				rows &= ~bit;
			}
		}
	}
}
