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

static inline bool
row_is_null(const PgBatchInt4Vector *column, int row)
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
row_value(const PgBatchInt4Vector *column, int row)
{
	if (column->layout == PG_BATCH_INT4_DATUM)
		return DatumGetInt32(column->data.datum.values[row]);
	return column->data.packed.values[column->data.packed.offset + row];
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
			if (!row_is_null(column, row)) \
			{ \
				value = row_value(column, row); \
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
not_null_mask(const PgBatchInt4Vector *column, int first_row, int nrows)
{
	uint64		result;

	if (column->layout == PG_BATCH_INT4_PACKED &&
		column->data.packed.validity == NULL)
		return word_mask(nrows, 0);
	if (column->layout == PG_BATCH_INT4_DATUM &&
		memchr(column->data.datum.isnull + first_row, true, nrows) == NULL)
		return word_mask(nrows, 0);

	result = 0;
	for (int row = 0; row < nrows; row++)
	{
		if (!row_is_null(column, first_row + row))
			result |= UINT64CONST(1) << row;
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
			passing &= not_null_mask(column, first_row, word_nrows);
			selection[word] = rows & passing;
		}
		else
			selection[word] = scalar_compare(column, first_row, rows,
										 op, scalar);
	}
}
