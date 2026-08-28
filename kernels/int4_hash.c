#include "postgres.h"

#include "common/hashfn.h"
#include "port/pg_bitutils.h"

#include "kernels.h"

static inline uint64
word_mask(int nrows, int word)
{
	int			remaining = nrows - word * 64;

	if (remaining >= 64)
		return UINT64_MAX;
	Assert(remaining > 0);
	return UINT64_MAX >> (64 - remaining);
}

static inline void
hash_value(uint32 *hashes, int row, int key, int32 value)
{
	uint32		key_hash = murmurhash32((uint32) value);

	hashes[row] = key == 0 ? key_hash : hash_combine(hashes[row], key_hash);
}

static void
hash_two_keys(const PgBatchInt4Vector *left,
			  const PgBatchInt4Vector *right,
			  int nrows, int nwords, const uint64 *selection,
			  uint32 *hashes, uint64 *valid_rows)
{
	for (int word = 0; word < nwords; word++)
	{
		uint64		rows = selection[word] & word_mask(nrows, word);
		int			first_row = word * 64;

		valid_rows[word] = 0;
		if (rows == 0)
			continue;
		if (left->layout == PG_BATCH_INT4_DATUM &&
			(word >= left->data.datum.nwords ||
			 (left->data.datum.available[word] & rows) != rows))
			elog(ERROR, "pg_batch int4 hash received unavailable Datum rows");
		if (right->layout == PG_BATCH_INT4_DATUM &&
			(word >= right->data.datum.nwords ||
			 (right->data.datum.available[word] & rows) != rows))
			elog(ERROR, "pg_batch int4 hash received unavailable Datum rows");
		while (rows != 0)
		{
			int			bitno = pg_rightmost_one_pos64(rows);
			uint64		bit = UINT64CONST(1) << bitno;
			int			row = first_row + bitno;

			if (!pg_batch_int4_row_is_null(left, row) &&
				!pg_batch_int4_row_is_null(right, row))
			{
				uint32		left_hash = murmurhash32((uint32)
					pg_batch_int4_row_value(left, row));
				uint32		right_hash = murmurhash32((uint32)
					pg_batch_int4_row_value(right, row));

				hashes[row] = hash_combine(left_hash, right_hash);
				valid_rows[word] |= bit;
			}
			rows &= ~bit;
		}
	}
}

void
pg_batch_hash_int4(const PgBatchInt4Vector *const *keys,
				   int nkeys, int nrows, int nwords,
				   const uint64 *selection, uint32 *hashes,
				   uint64 *valid_rows)
{
	Assert(nkeys > 0);
	Assert(nrows > 0);
	Assert(nwords == (nrows + 63) / 64);
	if (nkeys == 2)
	{
		hash_two_keys(keys[0], keys[1], nrows, nwords, selection,
					  hashes, valid_rows);
		return;
	}

	for (int word = 0; word < nwords; word++)
		valid_rows[word] = selection[word] & word_mask(nrows, word);

	for (int key = 0; key < nkeys; key++)
	{
		const PgBatchInt4Vector *column = keys[key];

		for (int word = 0; word < nwords; word++)
		{
			uint64		rows = valid_rows[word];
			int			first_row = word * 64;

			if (rows == 0)
				continue;
			if (column->layout == PG_BATCH_INT4_DATUM)
			{
				if (word >= column->data.datum.nwords ||
					(column->data.datum.available[word] & rows) != rows)
					elog(ERROR, "pg_batch int4 hash received unavailable Datum rows");
				while (rows != 0)
				{
					int			bitno = pg_rightmost_one_pos64(rows);
					uint64		bit = UINT64CONST(1) << bitno;
					int			row = first_row + bitno;

					if (column->data.datum.isnull[row])
						valid_rows[word] &= ~bit;
					else
						hash_value(hashes, row, key,
								   DatumGetInt32(column->data.datum.values[row]));
					rows &= ~bit;
				}
			}
			else
			{
				const int32 *values = column->data.packed.values +
					column->data.packed.offset + first_row;
				const uint8 *validity = column->data.packed.validity;

				while (rows != 0)
				{
					int			bitno = pg_rightmost_one_pos64(rows);
					uint64		bit = UINT64CONST(1) << bitno;
					int			row = first_row + bitno;
					int64		physical = column->data.packed.offset + row;

					if (validity != NULL &&
						(validity[physical / 8] &
						 ((uint8) 1 << (physical % 8))) == 0)
						valid_rows[word] &= ~bit;
					else
						hash_value(hashes, row, key, values[bitno]);
					rows &= ~bit;
				}
			}
		}
	}
}
