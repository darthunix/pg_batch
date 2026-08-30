/*
 * Common batch representation shared by independently built extensions.
 *
 * A PgBatch describes rows and their current selection. The node or source
 * owns the physical column storage and exposes it through PgBatchOps.
 */
#ifndef PG_BATCH_BATCH_H
#define PG_BATCH_BATCH_H

#include "postgres.h"

#include "executor/tuptable.h"
#include "nodes/bitmapset.h"
#include "port/pg_bitutils.h"

#define PG_BATCH_ABI_VERSION 1

/* Why columns are being prepared or converted to Datum values. */
typedef enum PgBatchColumnPhase
{
	PG_BATCH_COLUMN_FILTER,
	PG_BATCH_COLUMN_PROJECT
} PgBatchColumnPhase;

typedef struct PgBatch PgBatch;

/* Borrowed Datum view indexed by physical row number in the batch. */
typedef struct PgBatchDatumVector
{
	const Datum *values;
	const bool *isnull;
	const uint64 *valid_rows;
	int			nwords;
} PgBatchDatumVector;

/* Operations for the source-owned physical representation of a batch. */
typedef struct PgBatchOps
{
	uint32		abi_version;
	Size		struct_size;
	void		(*prepare_columns) (PgBatch *batch,
									const Bitmapset *columns,
									const uint64 *rows,
									PgBatchColumnPhase phase);
	void		(*get_datum_column) (PgBatch *batch, int column,
									 const uint64 *rows,
									 PgBatchColumnPhase phase,
									 PgBatchDatumVector *result);
	const void *(*get_native_interface) (PgBatch *batch,
										const char *name,
										uint32 version);
	void		(*release) (PgBatch *batch);
} PgBatchOps;

/*
 * Format-neutral envelope for one active batch.
 *
 * The owner keeps this structure, selection, operations, and private_data
 * alive until the batch is released. Bits beyond nrows must be clear.
 */
struct PgBatch
{
	uint32		abi_version;
	Size		struct_size;
	int			nrows;
	int			nwords;
	uint64	   *selection;
	Oid			table_oid;
	const PgBatchOps *ops;
	void	   *private_data;
};

/* Operations used to expose a published batch through a tuple slot. */
typedef struct PgBatchConsumerOps
{
	uint32		abi_version;
	Size		struct_size;
	void		(*accept_batch) (TupleTableSlot *slot, PgBatch *batch);
	void		(*select_row) (TupleTableSlot *slot, PgBatch *batch, int row);
} PgBatchConsumerOps;

static inline int
pg_batch_selection_count(const PgBatch *batch)
{
	int			result = 0;

	if (likely(batch->nwords == 1))
		return pg_popcount64(batch->selection[0]);
	for (int word = 0; word < batch->nwords; word++)
		result += pg_popcount64(batch->selection[word]);
	return result;
}

static inline bool
pg_batch_row_selected(const PgBatch *batch, int row)
{
	Assert(row >= 0 && row < batch->nrows);
	return (batch->selection[row / 64] &
			(UINT64CONST(1) << (row % 64))) != 0;
}

static inline void
pg_batch_clear_row(PgBatch *batch, int row)
{
	Assert(row >= 0 && row < batch->nrows);
	batch->selection[row / 64] &=
		~(UINT64CONST(1) << (row % 64));
}

/* Pass -1 to start; return -1 after the last selected row. */
static inline int
pg_batch_next_selected(const PgBatch *batch, int previous)
{
	int			row = previous + 1;
	int			word = row / 64;
	uint64		bits;

	if (row >= batch->nrows)
		return -1;
	bits = batch->selection[word] & (UINT64_MAX << (row % 64));
	for (;;)
	{
		if (bits != 0)
		{
			int			result = word * 64 + pg_rightmost_one_pos64(bits);

			return result < batch->nrows ? result : -1;
		}
		if (++word >= batch->nwords)
			return -1;
		bits = batch->selection[word];
	}
}

#endif /* PG_BATCH_BATCH_H */
