/*
 * Common batch representation shared by independently built extensions.
 *
 * A PgBatch describes rows and their current selection. The node or source
 * owns the physical column storage and exposes it through PgBatchOps.
 */
#ifndef PG_BATCH_BATCH_H
#define PG_BATCH_BATCH_H

#include "postgres.h"

#include "abi.h"
#include "executor/tuptable.h"
#include "nodes/bitmapset.h"
#include "port/pg_bitutils.h"

#define PG_BATCH_ABI_VERSION 1
#define PG_BATCH_OPS_ABI_VERSION 1
#define PG_BATCH_CONSUMER_OPS_ABI_VERSION 1

#define PG_BATCH_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatch, generation)
#define PG_BATCH_OPS_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchOps, get_datum_column)
#define PG_BATCH_CONSUMER_OPS_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchConsumerOps, select_row)

/* Why columns are being prepared or converted to Datum values. */
typedef enum PgBatchColumnPhase
{
	PG_BATCH_COLUMN_FILTER,
	PG_BATCH_COLUMN_PROJECT
} PgBatchColumnPhase;

typedef struct PgBatch PgBatch;

/* Common header of a typed source-native column interface. */
typedef struct PgBatchNativeInterface
{
	uint32		abi_version;
	Size		struct_size;
} PgBatchNativeInterface;

/* Stable descriptor used to discover one typed native interface. */
typedef struct PgBatchNativeType
{
	const char *name;
	uint32		abi_version;
	Size		min_size;
} PgBatchNativeType;

/* Borrowed Datum view indexed by physical row number in the batch. */
typedef struct PgBatchDatumVector
{
	/* Capacity supplied by the caller; providers must preserve this field. */
	Size		struct_size;
	const Datum *values;
	const bool *isnull;
	const uint64 *valid_rows;
	int			nwords;
} PgBatchDatumVector;

#define PG_BATCH_DATUM_VECTOR_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchDatumVector, nwords)

static inline void
pg_batch_check_datum_vector(const PgBatchDatumVector *result)
{
	if (result == NULL || result->struct_size < PG_BATCH_DATUM_VECTOR_MIN_SIZE)
		elog(ERROR, "pg_batch source received an incompatible Datum vector");
}

/* Operations for the source-owned physical representation of a batch. */
typedef struct PgBatchOps
{
	uint32		abi_version;
	Size		struct_size;
	/* Required v1 fallback available for every physical representation. */
	void		(*get_datum_column) (PgBatch *batch, int column,
									 const uint64 *rows,
									 PgBatchColumnPhase phase,
									 PgBatchDatumVector *result);
	/* Optional append-only operations. */
	void		(*prepare_columns) (PgBatch *batch,
									const Bitmapset *columns,
									const uint64 *rows,
									PgBatchColumnPhase phase);
	/* The returned interface depends only on this operations table. */
	const PgBatchNativeInterface *(*get_native_interface) (
										const PgBatchNativeType *type);
	void		(*release) (PgBatch *batch);
} PgBatchOps;

/*
 * Format-neutral envelope for one active batch.
 *
 * Publishing transfers exclusive use of this structure to the consumer. The
 * active consumer may prepare columns and may only remove rows from selection.
 * Passing the batch to a parent transfers that right again; it is not shared
 * ownership. The source keeps the referenced memory alive and serves calls
 * made through PgBatchOps, but must not otherwise mutate or reuse the active
 * batch until release() is called. Bits beyond nrows must be clear.
 *
 * Borrowed column views remain valid only until the owner next prepares or
 * releases the batch, unless the physical interface promises more.
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
	/* Set by the bridge while this batch is active, zero otherwise. */
	uint64		generation;
};

/* Operations used to expose a published batch through a tuple slot. */
typedef struct PgBatchConsumerOps
{
	uint32		abi_version;
	Size		struct_size;
	void		(*accept_batch) (TupleTableSlot *slot, PgBatch *batch);
	void		(*select_row) (TupleTableSlot *slot, PgBatch *batch, int row);
} PgBatchConsumerOps;

/*
 * Borrowed fast path for selecting rows from one active binding. The view is
 * valid only until that binding is finished, cleared, or given another batch.
 */
typedef struct PgBatchRowView
{
	Size		struct_size;
	TupleTableSlot *slot;
	PgBatch    *batch;
	uint64		generation;
	void		(*select_row) (TupleTableSlot *slot, PgBatch *batch, int row);
} PgBatchRowView;

#define PG_BATCH_ROW_VIEW_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchRowView, select_row)

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

static inline TupleTableSlot *
pg_batch_row_view_select(const PgBatchRowView *view, int row)
{
	if (view == NULL || view->struct_size < PG_BATCH_ROW_VIEW_MIN_SIZE ||
		view->batch == NULL || view->generation == 0 ||
		view->batch->generation != view->generation ||
		view->select_row == NULL ||
		row < 0 || row >= view->batch->nrows ||
		!pg_batch_row_selected(view->batch, row))
		elog(ERROR, "pg_batch cannot select row %d", row);
	view->select_row(view->slot, view->batch, row);
	return view->slot;
}

static inline void
pg_batch_clear_row(PgBatch *batch, int row)
{
	Assert(row >= 0 && row < batch->nrows);
	batch->selection[row / 64] &=
		~(UINT64CONST(1) << (row % 64));
}

/* Remove every row not present in rows; selected rows can never be restored. */
static inline void
pg_batch_intersect_selection(PgBatch *batch, const uint64 *rows)
{
	for (int word = 0; word < batch->nwords; word++)
		batch->selection[word] &= rows[word];
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
