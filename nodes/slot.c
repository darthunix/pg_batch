#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"

#include "internal.h"

static void
release_materialized(PgBatchSlot *bslot)
{
	if (bslot->materialized_tuple != NULL)
	{
		heap_freetuple(bslot->materialized_tuple);
		bslot->materialized_tuple = NULL;
	}
}

static void
slot_init(TupleTableSlot *slot)
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
slot_release(TupleTableSlot *slot)
{
	PgBatchSlot *bslot = (PgBatchSlot *) slot;

	pg_batch_bridge->detach_binding(bslot->binding);
	bslot->binding = NULL;
	bslot->active_batch = NULL;
	release_materialized(bslot);
	if (BufferIsValid(bslot->buffer))
		ReleaseBuffer(bslot->buffer);
	if (bslot->batch_context != NULL)
		MemoryContextDelete(bslot->batch_context);
}

static void
slot_clear(TupleTableSlot *slot)
{
	PgBatchSlot *bslot = (PgBatchSlot *) slot;

	pg_batch_bridge->clear_batch(bslot->binding);
	bslot->active_batch = NULL;
	release_materialized(bslot);
	if (BufferIsValid(bslot->buffer))
		ReleaseBuffer(bslot->buffer);
	bslot->buffer = InvalidBuffer;
	bslot->block = InvalidBlockNumber;
	bslot->current_row = -1;
	MemoryContextReset(bslot->batch_context);
	bslot->columns = NULL;
	slot->tts_nvalid = 0;
	slot->tts_flags |= TTS_FLAG_EMPTY;
	ItemPointerSetInvalid(&slot->tts_tid);
}

static HeapTupleData
heap_tuple(PgBatchSlot *bslot, int row)
{
	HeapTupleData tuple;
	Page		page;
	ItemId		item;
	OffsetNumber offset;

	Assert(BufferIsValid(bslot->buffer));
	Assert(row >= 0 && row < bslot->heap_batch.nrows);
	page = BufferGetPage(bslot->buffer);
	offset = bslot->item_offsets[row];
	item = PageGetItemId(page, offset);
	Assert(ItemIdIsNormal(item));

	tuple.t_len = ItemIdGetLength(item);
	tuple.t_data = (HeapTupleHeader) PageGetItem(page, item);
	ItemPointerSet(&tuple.t_self, bslot->block, offset);
	tuple.t_tableOid = bslot->heap_batch.table_oid;
	return tuple;
}

static pg_always_inline void
deform_attr(PgBatchSlot *bslot, HeapTuple tuple,
			AttrNumber source_attnum,
			HeapTupleDeformState * cursor, Datum *value, bool *isnull)
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
allocate_column(PgBatchSlot *bslot, int column)
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
store_deformed(PgBatchSlot *bslot, PgBatchColumn * bcolumn, int row,
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
materialize_row_side(PgBatchSlot *bslot, const int *columns,
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
			tuple = heap_tuple(bslot, row);
			tuple_loaded = true;
		}
		deform_attr(bslot, &tuple, source_attnum, work_cursor, &value,
					&isnull);
		store_deformed(bslot, bcolumn, row, value, isnull,
					   phase, restart_from_start);
	}
}

static void
materialize_filters(PgBatchSlot *bslot, const int *columns,
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
			tuple = heap_tuple(bslot, row);
			deform_attr(bslot, &tuple, source_attnum, work_cursor, &value,
						&isnull);
			store_deformed(bslot, bcolumn, row, value, isnull,
						   PG_BATCH_FILTER_PHASE, false);
			missing &= ~bit;
		}
	}
}

static void
heap_prepare_columns(PgBatchBridgeBatch *batch,
					 const Bitmapset *columns,
					 const uint64 *selected_rows,
					 PgBatchMaterializePhase phase)
{
	PgBatchSlot *bslot = batch->private_data;
	int			requested_columns[MaxTupleAttributeNumber];
	int			ncolumns = 0;
	int			column = -1;
	uint64		rows = selected_rows[0];

	Assert(batch->nwords == 1);
	if (columns == NULL || rows == 0)
		return;
	while ((column = bms_next_member(columns, column)) >= 0)
	{
		if (column >= bslot->ncolumns)
			elog(ERROR, "pg_batch column %d is out of range", column + 1);
		allocate_column(bslot, column);
		requested_columns[ncolumns++] = column;
	}
	if (phase == PG_BATCH_FILTER_PHASE)
	{
		/* Keep filter deformation column-major like filter evaluation. */
		materialize_filters(bslot, requested_columns, ncolumns, rows);
		return;
	}
	while (rows != 0)
	{
		int			row = pg_rightmost_one_pos64(rows);
		AttrNumber	cursor_attnum = bslot->cursors[row].nvalid;

		/*
		 * The saved cursor only moves forward. Columns before it use a local
		 * cursor; columns after it continue the saved cursor.
		 */
		if (cursor_attnum > 0)
			materialize_row_side(bslot, requested_columns, ncolumns,
								 row, phase, cursor_attnum, true);
		materialize_row_side(bslot, requested_columns, ncolumns, row,
							 phase, cursor_attnum, false);
		rows &= ~(UINT64CONST(1) << row);
	}
}

static void
heap_get_datum_column(PgBatchBridgeBatch *batch, int column,
					  const uint64 *selected_rows,
					  PgBatchMaterializePhase phase,
					  PgBatchBridgeDatumColumn *result)
{
	PgBatchSlot *bslot = batch->private_data;
	PgBatchColumn *bcolumn;
	uint64		missing;

	if (column < 0 || column >= bslot->ncolumns)
		elog(ERROR, "pg_batch column %d is out of range", column + 1);
	bcolumn = &bslot->columns[column];
	missing = selected_rows[0] & ~bcolumn->valid_rows;
	if (missing != 0)
	{
		Bitmapset  *columns = bms_make_singleton(column);

		heap_prepare_columns(batch, columns, selected_rows, phase);
		bms_free(columns);
	}
	result->values = bcolumn->values;
	result->isnull = bcolumn->isnull;
	result->valid_rows = &bcolumn->valid_rows;
	result->nwords = 1;
}

static void
heap_release(PgBatchBridgeBatch *batch)
{
	PgBatchSlot *bslot = batch->private_data;

	if (BufferIsValid(bslot->buffer))
		ReleaseBuffer(bslot->buffer);
	bslot->buffer = InvalidBuffer;
	bslot->block = InvalidBlockNumber;
}

static const PgBatchBridgeBatchOps pg_batch_heap_batch_ops = {
	.abi_version = PG_BATCH_BRIDGE_ABI_VERSION,
	.struct_size = sizeof(PgBatchBridgeBatchOps),
	.prepare_columns = heap_prepare_columns,
	.get_datum_column = heap_get_datum_column,
	.get_native_interface = NULL,
	.release = heap_release,
};

void
pg_batch_materialize_columns(PgBatchBridgeBatch *batch,
							 const Bitmapset *columns,
							 const uint64 *selected_rows,
							 PgBatchMaterializePhase phase)
{
	int			column = -1;

	while (columns != NULL &&
		   (column = bms_next_member(columns, column)) >= 0)
	{
		PgBatchBridgeDatumColumn ignored;

		pg_batch_get_datum_column(batch, column, selected_rows, phase,
								  &ignored);
	}
}

bool
pg_batch_get_arrow_column(PgBatchBridgeBatch *batch, int column,
						  PgBatchArrowView * result)
{
	const		PgBatchBridgeArrowInterface *arrow;

	if (batch->ops->get_native_interface == NULL)
		return false;
	arrow = batch->ops->get_native_interface(
											 batch, PG_BATCH_BRIDGE_ARROW_INTERFACE_NAME,
											 PG_BATCH_BRIDGE_ARROW_INTERFACE_VERSION);
	if (arrow == NULL)
		return false;
	if (arrow->abi_version != PG_BATCH_BRIDGE_ARROW_INTERFACE_VERSION ||
		arrow->struct_size < sizeof(PgBatchBridgeArrowInterface))
		elog(ERROR, "pg_batch source returned an incompatible Arrow interface");
	arrow->get_column(batch, column, result);
	return true;
}

static void consumer_select_row(TupleTableSlot *slot,
								PgBatchBridgeBatch *batch, int row);

static void
accept_batch(TupleTableSlot *slot, PgBatchBridgeBatch *batch)
{
	PgBatchSlot *bslot = pg_batch_slot_cast(slot);
	int			row = pg_batch_bridge_next_selected(batch, -1);

	if (row < 0)
		elog(ERROR, "pg_batch source published an empty selection");
	bslot->active_batch = batch;
	bslot->current_row = -1;
	/* publish_batch() calls us after the bridge has installed the batch. */
	consumer_select_row(slot, batch, row);
}

static void
consumer_select_row(TupleTableSlot *slot,
					PgBatchBridgeBatch *batch, int row)
{
	PgBatchSlot *bslot = pg_batch_slot_cast(slot);

	Assert(row >= 0 && row < batch->nrows);
	Assert(pg_batch_bridge_row_selected(batch, row));
	release_materialized(bslot);
	bslot->current_row = row;
	slot->tts_nvalid = 0;
	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	if (batch == &bslot->heap_batch && BufferIsValid(bslot->buffer))
		ItemPointerSet(&slot->tts_tid, bslot->block, bslot->item_offsets[row]);
	else
		ItemPointerSetInvalid(&slot->tts_tid);
	slot->tts_tableOid = batch->table_oid;
}

static const PgBatchBridgeConsumerOps pg_batch_slot_consumer_ops = {
	.abi_version = PG_BATCH_BRIDGE_ABI_VERSION,
	.struct_size = sizeof(PgBatchBridgeConsumerOps),
	.accept_batch = accept_batch,
	.select_row = consumer_select_row,
};

void
pg_batch_select_row(TupleTableSlot *slot, int row)
{
	PgBatchBridgeBatch *batch = pg_batch_get_batch(slot);

	if (!pg_batch_bridge_row_selected(batch, row))
		elog(ERROR, "pg_batch cannot select row %d", row);
	consumer_select_row(slot, batch, row);
}

static void
batch_slot_getsomeattrs(TupleTableSlot *slot, int natts)
{
	PgBatchSlot *bslot = (PgBatchSlot *) slot;
	PgBatchBridgeBatch *batch = pg_batch_get_batch(slot);
	Bitmapset  *needed = NULL;
	PgBatchMaterializePhase phase;
	uint64		local_rowbits;
	uint64	   *rowbits;

	if (bslot->current_row < 0 || bslot->current_row >= batch->nrows)
		elog(ERROR, "pg_batch slot has no current row (current %d, rows %d)",
			 bslot->current_row, batch->nrows);
	if (natts > bslot->ncolumns)
		elog(ERROR, "pg_batch requested too many attributes");

	if (slot->tts_nvalid < natts)
		needed = bms_add_range(needed, slot->tts_nvalid, natts - 1);
	phase = natts <= bslot->nfilter_columns ?
		PG_BATCH_FILTER_PHASE : PG_BATCH_PROJECT_PHASE;
	if (likely(batch->nwords == 1))
	{
		local_rowbits = UINT64CONST(1) << bslot->current_row;
		rowbits = &local_rowbits;
	}
	else
	{
		rowbits = palloc0_array(uint64, batch->nwords);
		rowbits[bslot->current_row / 64] =
			UINT64CONST(1) << (bslot->current_row % 64);
	}
	pg_batch_prepare_columns(batch, needed, rowbits, phase);

	for (int column = slot->tts_nvalid; column < natts; column++)
	{
		PgBatchBridgeDatumColumn bcolumn;
		int			word = bslot->current_row / 64;
		uint64		bit = UINT64CONST(1) << (bslot->current_row % 64);

		pg_batch_get_datum_column(batch, column, rowbits, phase, &bcolumn);
		if (word >= bcolumn.nwords || (bcolumn.valid_rows[word] & bit) == 0)
			elog(ERROR, "pg_batch source did not materialize column %d row %d",
				 column + 1, bslot->current_row);
		slot->tts_values[column] = bcolumn.values[bslot->current_row];
		slot->tts_isnull[column] = bcolumn.isnull[bslot->current_row];
	}
	slot->tts_nvalid = natts;
	bms_free(needed);
	if (batch->nwords > 1)
		pfree(rowbits);
}

static Datum
batch_slot_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	PgBatchSlot *bslot = (PgBatchSlot *) slot;
	HeapTupleData tuple;

	if (bslot->current_row < 0 || !BufferIsValid(bslot->buffer))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("system attributes are unavailable for this pg_batch source")));
	tuple = heap_tuple(bslot, bslot->current_row);
	return heap_getsysattr(&tuple, attnum, bslot->source_desc, isnull);
}

static bool
batch_slot_is_current_xact_tuple(TupleTableSlot *slot)
{
	PgBatchSlot *bslot = (PgBatchSlot *) slot;
	HeapTupleData tuple;
	TransactionId xmin;

	if (bslot->current_row < 0 || !BufferIsValid(bslot->buffer))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("transaction visibility metadata is unavailable for this pg_batch source")));
	tuple = heap_tuple(bslot, bslot->current_row);
	xmin = HeapTupleHeaderGetXmin(tuple.t_data);
	return TransactionIdIsCurrentTransactionId(xmin);
}

static void
slot_materialize(TupleTableSlot *slot)
{
	PgBatchSlot *bslot = (PgBatchSlot *) slot;
	MemoryContext oldcontext;

	if (bslot->materialized_tuple != NULL)
		return;
	batch_slot_getsomeattrs(slot, slot->tts_tupleDescriptor->natts);
	oldcontext = MemoryContextSwitchTo(slot->tts_mcxt);
	bslot->materialized_tuple =
		heap_form_tuple(slot->tts_tupleDescriptor,
						slot->tts_values, slot->tts_isnull);
	MemoryContextSwitchTo(oldcontext);
}

static void
slot_copyslot(TupleTableSlot *dstslot, TupleTableSlot *srcslot)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("pg_batch slots cannot be copy destinations")));
}

static HeapTuple
slot_get_heap_tuple(TupleTableSlot *slot)
{
	slot_materialize(slot);
	return ((PgBatchSlot *) slot)->materialized_tuple;
}

static HeapTuple
slot_copy_heap_tuple(TupleTableSlot *slot)
{
	return heap_copytuple(slot_get_heap_tuple(slot));
}

static MinimalTuple
slot_copy_minimal_tuple(TupleTableSlot *slot, Size extra)
{
	HeapTuple	tuple = slot_get_heap_tuple(slot);

	return minimal_tuple_from_heap_tuple(tuple, extra);
}

const TupleTableSlotOps PgBatchSlotOps = {
	.base_slot_size = sizeof(PgBatchSlot),
	.init = slot_init,
	.release = slot_release,
	.clear = slot_clear,
	.getsomeattrs = batch_slot_getsomeattrs,
	.getsysattr = batch_slot_getsysattr,
	.is_current_xact_tuple = batch_slot_is_current_xact_tuple,
	.materialize = slot_materialize,
	.copyslot = slot_copyslot,
	.get_heap_tuple = slot_get_heap_tuple,
	.copy_heap_tuple = slot_copy_heap_tuple,
	.copy_minimal_tuple = slot_copy_minimal_tuple,
};

void
pg_batch_configure_slot(PgBatchSlot *bslot, TupleDesc source_desc,
						List *source_attnums, int nfilter_columns)
{
	ListCell   *lc;
	int			column = 0;

	bslot->source_desc = source_desc;
	bslot->ncolumns = list_length(source_attnums);
	if (nfilter_columns < 0 || nfilter_columns > bslot->ncolumns)
		elog(ERROR, "invalid pg_batch slot configuration");
	bslot->nfilter_columns = nfilter_columns;
	bslot->source_attnums = palloc_array(AttrNumber, bslot->ncolumns);
	foreach(lc, source_attnums)
		bslot->source_attnums[column++] = lfirst_int(lc);
	Assert(column == bslot->ncolumns);
	bslot->binding = pg_batch_bridge->attach_slot(&bslot->base,
												  bslot->source_attnums,
												  bslot->ncolumns,
												  &pg_batch_slot_consumer_ops);
}

void
pg_batch_set_request(TupleTableSlot *slot, const Bitmapset *filter_columns,
					 const Bitmapset *survivor_columns, bool return_batch)
{
	PgBatchSlot *bslot = pg_batch_slot_cast(slot);

	pg_batch_bridge->set_request(bslot->binding, filter_columns,
								 survivor_columns, return_batch);
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
	memcpy(bslot->item_offsets, item_offsets, sizeof(OffsetNumber) * nrows);
	bslot->current_row = -1;
	memset(bslot->cursors, 0, sizeof(HeapTupleDeformState) * nrows);

	bslot->columns = MemoryContextAllocZero(bslot->batch_context,
											sizeof(PgBatchColumn) * bslot->ncolumns);

	bslot->heap_selection = pg_batch_nrows_mask(nrows);
	bslot->heap_batch.abi_version = PG_BATCH_BRIDGE_ABI_VERSION;
	bslot->heap_batch.struct_size = sizeof(PgBatchBridgeBatch);
	bslot->heap_batch.nrows = nrows;
	bslot->heap_batch.nwords = 1;
	bslot->heap_batch.selection = &bslot->heap_selection;
	bslot->heap_batch.table_oid = table_oid;
	bslot->heap_batch.ops = &pg_batch_heap_batch_ops;
	bslot->heap_batch.private_data = bslot;
	pg_batch_bridge->publish_batch(bslot->binding, &bslot->heap_batch);
}
