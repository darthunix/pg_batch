#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"

#include "pg_batch.h"

static void
pg_batch_slot_release_materialized(PgBatchSlot *bslot)
{
	if (bslot->materialized_tuple != NULL)
	{
		heap_freetuple(bslot->materialized_tuple);
		bslot->materialized_tuple = NULL;
	}
}

static void
pg_batch_slot_init(TupleTableSlot *slot)
{
	PgBatchSlot *bslot = (PgBatchSlot *) slot;

	bslot->buffer = InvalidBuffer;
	bslot->block = InvalidBlockNumber;
	bslot->current_row = -1;
	bslot->batch_context =
		AllocSetContextCreate(slot->tts_mcxt, "pg_batch tuple batch",
							  ALLOCSET_DEFAULT_SIZES);
}

static void
pg_batch_slot_release(TupleTableSlot *slot)
{
	PgBatchSlot *bslot = (PgBatchSlot *) slot;

	pg_batch_compressed_scan_end(bslot);
	pg_batch_slot_release_materialized(bslot);
	if (BufferIsValid(bslot->buffer))
		ReleaseBuffer(bslot->buffer);
	if (bslot->batch_context != NULL)
		MemoryContextDelete(bslot->batch_context);
}

static void
pg_batch_slot_clear(TupleTableSlot *slot)
{
	PgBatchSlot *bslot = (PgBatchSlot *) slot;

	pg_batch_slot_release_materialized(bslot);
	if (unlikely(bslot->format_ops != NULL &&
				 bslot->format_ops->clear_batch != NULL))
		bslot->format_ops->clear_batch(bslot);
	if (BufferIsValid(bslot->buffer))
		ReleaseBuffer(bslot->buffer);
	bslot->buffer = InvalidBuffer;
	bslot->block = InvalidBlockNumber;
	bslot->nrows = 0;
	bslot->current_row = -1;
	bslot->selected_rows = 0;
	bslot->batch_consumed = false;
	bslot->format_ops = NULL;
	bslot->format_private = NULL;
	MemoryContextReset(bslot->batch_context);
	bslot->columns = NULL;
	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
}

static HeapTupleData
pg_batch_heap_tuple(PgBatchSlot *bslot, int row)
{
	HeapTupleData tuple;
	Page		page;
	ItemId		item;
	OffsetNumber offset;

	Assert(BufferIsValid(bslot->buffer));
	Assert(row >= 0 && row < bslot->nrows);
	page = BufferGetPage(bslot->buffer);
	offset = bslot->item_offsets[row];
	item = PageGetItemId(page, offset);
	Assert(ItemIdIsNormal(item));

	tuple.t_len = ItemIdGetLength(item);
	tuple.t_data = (HeapTupleHeader) PageGetItem(page, item);
	ItemPointerSet(&tuple.t_self, bslot->block, offset);
	tuple.t_tableOid = bslot->table_oid;
	return tuple;
}

static pg_always_inline void
pg_batch_deform_attr(PgBatchSlot *bslot, HeapTuple tuple,
					 AttrNumber source_attnum,
					 HeapTupleDeformState *cursor, Datum *value, bool *isnull)
{
	int			first_non_guaranteed_attr =
		bslot->source_desc->firstNonGuaranteedAttr;

	if (unlikely(!heap_deform_tuple_advance(cursor, tuple, bslot->source_desc,
											first_non_guaranteed_attr,
											source_attnum, value, isnull, false)))
	{
		/* The planner rejects relations with non-NULL missing values. */
		*value = (Datum) 0;
		*isnull = true;
	}
}

static void
pg_batch_allocate_column(PgBatchSlot *bslot, int column)
{
	PgBatchColumn *bcolumn = &bslot->columns[column];

	if (bcolumn->values != NULL)
		return;

	bcolumn->values = MemoryContextAlloc(bslot->batch_context,
										 sizeof(Datum) * PG_BATCH_SIZE);
	bcolumn->isnull = MemoryContextAlloc(bslot->batch_context,
										 sizeof(bool) * PG_BATCH_SIZE);
}

static void
pg_batch_store_deformed(PgBatchSlot *bslot, PgBatchColumn *bcolumn, int row,
						Datum value, bool isnull,
						PgBatchMaterializePhase phase, bool restarted)
{
	bcolumn->values[row] = value;
	bcolumn->isnull[row] = isnull;
	bcolumn->valid_rows |= UINT64CONST(1) << row;
	if (phase == PG_BATCH_FILTER_PHASE)
		bslot->filter_datums++;
	else
	{
		bslot->project_datums++;
		if (restarted)
			bslot->restarted_project_datums++;
	}
}

static void
pg_batch_materialize_row_side(PgBatchSlot *bslot, const int *columns,
							  int ncolumns, int row,
							  PgBatchMaterializePhase phase,
							  AttrNumber cursor_attnum,
							  bool restart_from_start)
{
	HeapTupleDeformState *cursor = &bslot->cursors[row];
	HeapTupleDeformState local;
	HeapTupleDeformState *work_cursor = restart_from_start ? &local : cursor;
	HeapTupleData tuple;
	uint64		bit = UINT64CONST(1) << row;
	bool		tuple_loaded = false;

	if (restart_from_start)
		heap_deform_tuple_init(&local);

	for (int i = 0; i < ncolumns; i++)
	{
		int			column = columns[i];
		PgBatchColumn *bcolumn = &bslot->columns[column];
		AttrNumber	source_attnum = bslot->source_attnums[column];
		Datum		value;
		bool		isnull;

		if ((bcolumn->valid_rows & bit) != 0)
			continue;
		if (restart_from_start)
		{
			if (source_attnum > cursor_attnum)
				continue;
		}
		else if (source_attnum <= cursor_attnum)
			continue;

		if (source_attnum <= work_cursor->nvalid)
			elog(ERROR, "pg_batch columns are not in physical order");
		if (!tuple_loaded)
		{
			tuple = pg_batch_heap_tuple(bslot, row);
			tuple_loaded = true;
		}
		pg_batch_deform_attr(bslot, &tuple, source_attnum, work_cursor, &value,
							 &isnull);
		pg_batch_store_deformed(bslot, bcolumn, row, value, isnull,
								phase, restart_from_start);
	}
}

static void
pg_batch_materialize_filters(PgBatchSlot *bslot, const int *columns,
							 int ncolumns, uint64 rows)
{
	for (int i = 0; i < ncolumns; i++)
	{
		int			column = columns[i];
		PgBatchColumn *bcolumn = &bslot->columns[column];
		AttrNumber	source_attnum = bslot->source_attnums[column];
		uint64		missing = rows & ~bcolumn->valid_rows;

		while (missing != 0)
		{
			int			row = pg_rightmost_one_pos64(missing);
			uint64		bit = UINT64CONST(1) << row;
			HeapTupleDeformState local;
			HeapTupleDeformState *cursor = &bslot->cursors[row];
			HeapTupleDeformState *work_cursor = cursor;
			HeapTupleData tuple;
			Datum		value;
			bool		isnull;

			if (source_attnum <= cursor->nvalid)
			{
				heap_deform_tuple_init(&local);
				work_cursor = &local;
			}
			tuple = pg_batch_heap_tuple(bslot, row);
			pg_batch_deform_attr(bslot, &tuple, source_attnum, work_cursor, &value,
								 &isnull);
			pg_batch_store_deformed(bslot, bcolumn, row, value, isnull,
									PG_BATCH_FILTER_PHASE, false);
			missing &= ~bit;
		}
	}
}

void
pg_batch_prepare_columns(PgBatchSlot *bslot, const Bitmapset *columns,
						 uint64 selected_rows,
						 PgBatchMaterializePhase phase)
{
	int			requested_columns[MaxTupleAttributeNumber];
	int			ncolumns = 0;
	int			column = -1;

	if (columns == NULL || selected_rows == 0)
		return;
	if (unlikely(bslot->format_ops != NULL))
	{
		if (bslot->format_ops->prepare_columns == NULL)
			elog(ERROR, "pg_batch slot has no active batch format");
		bslot->format_ops->prepare_columns(bslot, columns, selected_rows, phase);
		return;
	}

	while ((column = bms_next_member(columns, column)) >= 0)
	{
		if (column >= bslot->ncolumns)
			elog(ERROR, "pg_batch column %d is out of range", column + 1);
		pg_batch_allocate_column(bslot, column);
		requested_columns[ncolumns++] = column;
	}
	if (phase == PG_BATCH_FILTER_PHASE)
	{
		/* Keep filter deformation column-major like filter evaluation. */
		pg_batch_materialize_filters(bslot, requested_columns, ncolumns,
									 selected_rows);
		return;
	}
	while (selected_rows != 0)
	{
		int			row = pg_rightmost_one_pos64(selected_rows);
		AttrNumber	cursor_attnum = bslot->cursors[row].nvalid;

		/*
		 * The saved cursor only moves forward. Columns before it use a local
		 * cursor; columns after it continue the saved cursor.
		 */
		if (cursor_attnum > 0)
			pg_batch_materialize_row_side(bslot, requested_columns, ncolumns,
										  row, phase, cursor_attnum, true);
		pg_batch_materialize_row_side(bslot, requested_columns, ncolumns, row,
									  phase, cursor_attnum, false);
		selected_rows &= ~(UINT64CONST(1) << row);
	}
}

PgBatchArrowView
pg_batch_get_arrow_column(PgBatchSlot *bslot, int column)
{
	if (bslot->format_ops == NULL ||
		bslot->format_ops->get_arrow_column == NULL)
		elog(ERROR, "pg_batch slot has no active batch format");
	return bslot->format_ops->get_arrow_column(bslot, column);
}

void
pg_batch_materialize_columns(PgBatchSlot *bslot, const Bitmapset *columns,
							 uint64 selected_rows,
							 PgBatchMaterializePhase phase)
{
	int			column = -1;

	pg_batch_prepare_columns(bslot, columns, selected_rows, phase);
	if (bslot->format_ops == NULL)
		return;
	while (columns != NULL &&
		   (column = bms_next_member(columns, column)) >= 0)
	{
		PgBatchArrowView view = pg_batch_get_arrow_column(bslot, column);
		PgBatchColumn *datum_column;
		const struct ArrowArray *array;
		const int32 *values;
		uint64		missing;

		array = view.array;
		Assert(strcmp(view.schema->format, "i") == 0);
		Assert(array->offset >= 0 && array->length == bslot->nrows);
		pg_batch_allocate_column(bslot, column);
		datum_column = &bslot->columns[column];
		values = array->buffers[1];
		missing = selected_rows & ~datum_column->valid_rows;
		while (missing != 0)
		{
			int			row = pg_rightmost_one_pos64(missing);
			uint64		bit = UINT64CONST(1) << row;
			bool		isnull = !pg_batch_arrow_row_is_valid(array, row);
			Datum		value = isnull ? (Datum) 0 :
				Int32GetDatum(values[array->offset + row]);

			pg_batch_store_deformed(bslot, datum_column, row, value, isnull,
									phase, false);
			missing &= ~bit;
		}
	}
}

void
pg_batch_slot_select_row(PgBatchSlot *bslot, int row)
{
	TupleTableSlot *slot = &bslot->base;

	Assert(row >= 0 && row < bslot->nrows);
	Assert((bslot->selected_rows & (UINT64CONST(1) << row)) != 0);
	pg_batch_slot_release_materialized(bslot);
	bslot->current_row = row;
	slot->tts_nvalid = 0;
	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	if (BufferIsValid(bslot->buffer))
		ItemPointerSet(&slot->tts_tid, bslot->block, bslot->item_offsets[row]);
	else
		ItemPointerSetInvalid(&slot->tts_tid);
	slot->tts_tableOid = bslot->table_oid;
}

static void
pg_batch_slot_getsomeattrs(TupleTableSlot *slot, int natts)
{
	PgBatchSlot *bslot = (PgBatchSlot *) slot;
	Bitmapset  *needed = NULL;
	uint64		rowbit;

	if (bslot->current_row < 0 || bslot->current_row >= bslot->nrows)
		elog(ERROR, "pg_batch slot has no current row (current %d, rows %d)",
			 bslot->current_row, bslot->nrows);
	if (natts > bslot->ncolumns)
		elog(ERROR, "pg_batch requested too many attributes");

	for (int column = slot->tts_nvalid; column < natts; column++)
		needed = bms_add_member(needed, column);
	rowbit = UINT64CONST(1) << bslot->current_row;
	pg_batch_materialize_columns(bslot, needed, rowbit,
								 natts <= bslot->nfilter_columns ?
								 PG_BATCH_FILTER_PHASE : PG_BATCH_PROJECT_PHASE);

	for (int column = slot->tts_nvalid; column < natts; column++)
	{
		PgBatchColumn *bcolumn = &bslot->columns[column];

		Assert((bcolumn->valid_rows & rowbit) != 0);
		slot->tts_values[column] = bcolumn->values[bslot->current_row];
		slot->tts_isnull[column] = bcolumn->isnull[bslot->current_row];
	}
	slot->tts_nvalid = natts;
	bms_free(needed);
}

static Datum
pg_batch_slot_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	PgBatchSlot *bslot = (PgBatchSlot *) slot;
	HeapTupleData tuple;

	if (bslot->current_row < 0 || !BufferIsValid(bslot->buffer))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("system attributes are unavailable for this pg_batch source")));
	tuple = pg_batch_heap_tuple(bslot, bslot->current_row);
	return heap_getsysattr(&tuple, attnum, bslot->source_desc, isnull);
}

static bool
pg_batch_slot_is_current_xact_tuple(TupleTableSlot *slot)
{
	PgBatchSlot *bslot = (PgBatchSlot *) slot;
	HeapTupleData tuple;
	TransactionId xmin;

	if (bslot->current_row < 0 || !BufferIsValid(bslot->buffer))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("transaction visibility metadata is unavailable for this pg_batch source")));
	tuple = pg_batch_heap_tuple(bslot, bslot->current_row);
	xmin = HeapTupleHeaderGetXmin(tuple.t_data);
	return TransactionIdIsCurrentTransactionId(xmin);
}

static void
pg_batch_slot_materialize(TupleTableSlot *slot)
{
	PgBatchSlot *bslot = (PgBatchSlot *) slot;
	MemoryContext oldcontext;

	if (bslot->materialized_tuple != NULL)
		return;
	pg_batch_slot_getsomeattrs(slot, slot->tts_tupleDescriptor->natts);
	oldcontext = MemoryContextSwitchTo(slot->tts_mcxt);
	bslot->materialized_tuple =
		heap_form_tuple(slot->tts_tupleDescriptor,
						slot->tts_values, slot->tts_isnull);
	MemoryContextSwitchTo(oldcontext);
}

static void
pg_batch_slot_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("pg_batch slots cannot be copy destinations")));
}

static HeapTuple
pg_batch_slot_get_heap_tuple(TupleTableSlot *slot)
{
	pg_batch_slot_materialize(slot);
	return ((PgBatchSlot *) slot)->materialized_tuple;
}

static HeapTuple
pg_batch_slot_copy_heap_tuple(TupleTableSlot *slot)
{
	return heap_copytuple(pg_batch_slot_get_heap_tuple(slot));
}

static MinimalTuple
pg_batch_slot_copy_minimal_tuple(TupleTableSlot *slot, Size extra)
{
	HeapTuple	tuple = pg_batch_slot_get_heap_tuple(slot);

	return minimal_tuple_from_heap_tuple(tuple, extra);
}

const TupleTableSlotOps PgBatchSlotOps = {
	.base_slot_size = sizeof(PgBatchSlot),
	.init = pg_batch_slot_init,
	.release = pg_batch_slot_release,
	.clear = pg_batch_slot_clear,
	.getsomeattrs = pg_batch_slot_getsomeattrs,
	.getsysattr = pg_batch_slot_getsysattr,
	.is_current_xact_tuple = pg_batch_slot_is_current_xact_tuple,
	.materialize = pg_batch_slot_materialize,
	.copyslot = pg_batch_slot_copyslot,
	.get_heap_tuple = pg_batch_slot_get_heap_tuple,
	.copy_heap_tuple = pg_batch_slot_copy_heap_tuple,
	.copy_minimal_tuple = pg_batch_slot_copy_minimal_tuple,
};

void
pg_batch_configure_slot(PgBatchSlot *bslot, TupleDesc source_desc,
						List *source_attnums, int nfilter_columns)
{
	ListCell   *lc;
	int			column = 0;

	bslot->source_desc = source_desc;
	bslot->ncolumns = list_length(source_attnums);
	bslot->nfilter_columns = nfilter_columns;
	bslot->source_attnums = palloc_array(AttrNumber, bslot->ncolumns);
	foreach(lc, source_attnums)
		bslot->source_attnums[column++] = lfirst_int(lc);
	Assert(column == bslot->ncolumns);
}

void
pg_batch_set_request(PgBatchSlot *bslot, const Bitmapset *filter_columns,
					 const Bitmapset *survivor_columns, bool return_batch)
{
	Bitmapset  *new_filter_columns = bms_copy(filter_columns);
	Bitmapset  *new_survivor_columns = bms_copy(survivor_columns);

	bms_free(bslot->request.filter_columns);
	bms_free(bslot->request.survivor_columns);
	bslot->request.filter_columns = new_filter_columns;
	bslot->request.survivor_columns = new_survivor_columns;
	bslot->request.return_batch = return_batch;
}

void
pg_batch_set_source_request(PgBatchSlot *bslot,
							const PgBatchSourceQual *quals, int nquals,
							PgBatchCompressedScanMode mode)
{
	bslot->request.source_quals = quals;
	bslot->request.nsource_quals = nquals;
	bslot->request.source_mode = mode;
}

void
pg_batch_load_batch(PgBatchSlot *bslot, Buffer buffer, BlockNumber block,
					Oid table_oid, const OffsetNumber *item_offsets, int nrows)
{
	TupleTableSlot *slot = &bslot->base;

	ExecClearTuple(slot);
	Assert(BufferIsValid(buffer));
	Assert(nrows > 0 && nrows <= PG_BATCH_SIZE);
	IncrBufferRefCount(buffer);
	bslot->buffer = buffer;
	bslot->block = block;
	bslot->table_oid = table_oid;
	bslot->format_ops = NULL;
	bslot->format_private = NULL;
	memcpy(bslot->item_offsets, item_offsets, sizeof(OffsetNumber) * nrows);
	bslot->nrows = nrows;
	bslot->selected_rows = pg_batch_nrows_mask(nrows);
	bslot->current_row = 0;
	bslot->batch_consumed = false;
	memset(bslot->cursors, 0, sizeof(HeapTupleDeformState) * nrows);

	bslot->columns = MemoryContextAllocZero(bslot->batch_context,
											sizeof(PgBatchColumn) * bslot->ncolumns);

	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	pg_batch_slot_select_row(bslot, 0);
}
