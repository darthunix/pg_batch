#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "pg_batch.h"

PG_FUNCTION_INFO_V1(pg_batch_compress);

#define PG_BATCH_DEFAULT_GROUP_ROWS 4096
#define PG_BATCH_DISTINCT_VALUES 16

typedef enum PgBatchCompression
{
	PG_BATCH_COMPRESS_PLAIN,
	PG_BATCH_COMPRESS_DELTA8,
	PG_BATCH_COMPRESS_DELTA16
} PgBatchCompression;

typedef struct PgBatchCompressedColumn
{
	PgBatchCompression encoding;
	uint8		validity[PG_BATCH_SIZE / 8];
	uint16		null_count;
	int32		base;
	uint32		data_offset;
} PgBatchCompressedColumn;

typedef struct PgBatchCompressedBatch
{
	int			nrows;
	Size		bytes;
	PgBatchCompressedColumn columns[FLEXIBLE_ARRAY_MEMBER];
} PgBatchCompressedBatch;

typedef struct PgBatchGroupColumn
{
	/* Coarse metadata belongs to a storage group, not a 64-row batch. */
	bool		has_nonnull;
	bool		has_null;
	/* distinct[] is exact until more than this small fixed set is seen. */
	bool		distinct_overflow;
	uint8		ndistinct;
	int32		minimum;
	int32		maximum;
	int32		distinct[PG_BATCH_DISTINCT_VALUES];
} PgBatchGroupColumn;

typedef struct PgBatchCompressedGroup
{
	int			first_batch;
	int			nbatches;
	PgBatchGroupColumn columns[FLEXIBLE_ARRAY_MEMBER];
} PgBatchCompressedGroup;

typedef struct PgBatchCompressedRelation PgBatchCompressedRelation;

struct PgBatchCompressedRelation
{
	MemoryContext context;
	Oid			relid;
	int			ncolumns;
	int			nbatches;
	int			batch_capacity;
	int			ngroups;
	int			group_capacity;
	int			group_batches;
	uint64		bytes;
	int			refcount;
	PgBatchCompressedBatch **batches;
	PgBatchCompressedGroup **groups;
	PgBatchCompressedRelation *next;
};

typedef struct PgBatchArrowColumn
{
	struct ArrowArray array;
	struct ArrowSchema schema;
	const void *buffers[2];
} PgBatchArrowColumn;

typedef struct PgBatchCompressedActive
{
	PgBatchCompressedBatch *batch;
	PgBatchArrowColumn columns[FLEXIBLE_ARRAY_MEMBER];
} PgBatchCompressedActive;

typedef struct PgBatchCompressedScan
{
	PgBatchCompressedRelation *relation;
	int			group_index;
	int			batch_index;
	bool			group_ready;
	PgBatchCompressedStats stats;
} PgBatchCompressedScan;

static PgBatchCompressedRelation *compressed_relations;

static inline void
pg_batch_arrow_set_valid(uint8 *validity, int row)
{
	validity[row / 8] |= (uint8) 1 << (row % 8);
}

static void
pg_batch_arrow_array_release(struct ArrowArray *array)
{
	array->release = NULL;
}

static void
pg_batch_arrow_schema_release(struct ArrowSchema *schema)
{
	schema->release = NULL;
}

static bool
pg_batch_compressed_relation_supported(Relation relation, bool report_error)
{
	TupleDesc	desc = RelationGetDescr(relation);

	if (relation->rd_rel->relkind != RELKIND_RELATION ||
		(relation->rd_rel->relam != HEAP_TABLE_AM_OID &&
		 !pg_batch_relation_uses_tableam(relation)))
	{
		if (report_error)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("pg_batch_compress supports only heap-compatible tables")));
		return false;
	}
	if (desc->natts == 0)
	{
		if (report_error)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("pg_batch_compress requires at least one column")));
		return false;
	}
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		if (attr->attisdropped || attr->atthasmissing ||
			attr->atttypid != INT4OID)
		{
			if (report_error)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("pg_batch_compress supports only int4 columns without dropped or missing values")));
			return false;
		}
	}
	return true;
}

static PgBatchCompression
pg_batch_choose_compression(const int32 *values, uint64 nonnull_rows,
							int nrows, int32 *base, Size *data_size)
{
	int32		normalized[PG_BATCH_SIZE];
	bool		fits_int8 = true;
	bool		fits_int16 = true;
	int32		last = 0;

	for (int row = 0; row < nrows; row++)
	{
		if ((nonnull_rows & (UINT64CONST(1) << row)) != 0)
			last = values[row];
		normalized[row] = last;
	}
	*base = normalized[0];
	for (int row = 1; row < nrows; row++)
	{
		int64		delta = (int64) normalized[row] - normalized[row - 1];

		if (delta < PG_INT8_MIN || delta > PG_INT8_MAX)
			fits_int8 = false;
		if (delta < PG_INT16_MIN || delta > PG_INT16_MAX)
			fits_int16 = false;
	}
	if (fits_int8)
	{
		*data_size = nrows - 1;
		return PG_BATCH_COMPRESS_DELTA8;
	}
	if (fits_int16)
	{
		*data_size = sizeof(int16) * (nrows - 1);
		return PG_BATCH_COMPRESS_DELTA16;
	}
	*data_size = sizeof(int32) * nrows;
	return PG_BATCH_COMPRESS_PLAIN;
}

static void
pg_batch_write_column(PgBatchCompressedBatch *batch, int column,
					  const int32 *values, uint64 nonnull_rows, int nrows)
{
	PgBatchCompressedColumn *stored = &batch->columns[column];
	char	   *output = (char *) batch + stored->data_offset;
	int32		last = 0;

	for (int row = 0; row < nrows; row++)
	{
		if ((nonnull_rows & (UINT64CONST(1) << row)) != 0)
			pg_batch_arrow_set_valid(stored->validity, row);
		else
			stored->null_count++;
	}
	if (stored->encoding == PG_BATCH_COMPRESS_PLAIN)
	{
		memcpy(output, values, sizeof(int32) * nrows);
		return;
	}
	last = stored->base;
	for (int row = 1; row < nrows; row++)
	{
		int32		value = last;
		int64		delta;

		if ((nonnull_rows & (UINT64CONST(1) << row)) != 0)
			value = values[row];
		delta = (int64) value - last;
		if (stored->encoding == PG_BATCH_COMPRESS_DELTA8)
			((int8 *) output)[row - 1] = (int8) delta;
		else
			((int16 *) output)[row - 1] = (int16) delta;
		last = value;
	}
}

static PgBatchCompressedBatch *
pg_batch_build_compressed_batch(const int32 *values,
								const uint64 *nonnull_rows,
								int ncolumns, int nrows)
{
	PgBatchCompression *encodings = palloc_array(PgBatchCompression, ncolumns);
	Size	   *data_sizes = palloc_array(Size, ncolumns);
	int32	   *bases = palloc_array(int32, ncolumns);
	Size		header_size = offsetof(PgBatchCompressedBatch, columns) +
		sizeof(PgBatchCompressedColumn) * ncolumns;
	Size		bytes = MAXALIGN(header_size);
	PgBatchCompressedBatch *batch;

	for (int column = 0; column < ncolumns; column++)
	{
		encodings[column] =
			pg_batch_choose_compression(values + column * PG_BATCH_SIZE,
										nonnull_rows[column], nrows,
										&bases[column], &data_sizes[column]);
		bytes = add_size(bytes, MAXALIGN(data_sizes[column]));
	}
	batch = palloc0(bytes);
	batch->nrows = nrows;
	batch->bytes = bytes;
	header_size = MAXALIGN(header_size);
	for (int column = 0; column < ncolumns; column++)
	{
		PgBatchCompressedColumn *stored = &batch->columns[column];

		stored->encoding = encodings[column];
		stored->base = bases[column];
		stored->data_offset = header_size;
		pg_batch_write_column(batch, column,
							  values + column * PG_BATCH_SIZE,
							  nonnull_rows[column], nrows);
		header_size += MAXALIGN(data_sizes[column]);
	}
	pfree(encodings);
	pfree(data_sizes);
	pfree(bases);
	return batch;
}

static PgBatchCompressedGroup *
pg_batch_append_group(PgBatchCompressedRelation *compressed)
{
	Size		bytes = offsetof(PgBatchCompressedGroup, columns) +
		sizeof(PgBatchGroupColumn) * compressed->ncolumns;
	PgBatchCompressedGroup *group;

	if (compressed->ngroups == compressed->group_capacity)
	{
		compressed->group_capacity *= 2;
		compressed->groups = repalloc_array(compressed->groups,
											 PgBatchCompressedGroup *,
											 compressed->group_capacity);
	}
	group = palloc0(bytes);
	group->first_batch = compressed->nbatches;
	compressed->groups[compressed->ngroups++] = group;
	compressed->bytes += bytes;
	return group;
}

static void
pg_batch_update_group(PgBatchCompressedGroup *group, const int32 *values,
					  const uint64 *nonnull_rows, int ncolumns, int nrows)
{
	for (int column = 0; column < ncolumns; column++)
	{
		PgBatchGroupColumn *meta = &group->columns[column];

		for (int row = 0; row < nrows; row++)
		{
			uint64		bit = UINT64CONST(1) << row;
			int32		value;
			bool		known = false;

			if ((nonnull_rows[column] & bit) == 0)
			{
				meta->has_null = true;
				continue;
			}
			value = values[column * PG_BATCH_SIZE + row];
			if (!meta->has_nonnull)
			{
				meta->has_nonnull = true;
				meta->minimum = meta->maximum = value;
			}
			else
			{
				meta->minimum = Min(meta->minimum, value);
				meta->maximum = Max(meta->maximum, value);
			}

			if (meta->distinct_overflow)
				continue;
			for (int i = 0; i < meta->ndistinct; i++)
			{
				if (meta->distinct[i] == value)
				{
					known = true;
					break;
				}
			}
			if (!known && meta->ndistinct < PG_BATCH_DISTINCT_VALUES)
				meta->distinct[meta->ndistinct++] = value;
			else if (!known)
				meta->distinct_overflow = true;
		}
	}
	group->nbatches++;
}

static void
pg_batch_append_compressed_batch(PgBatchCompressedRelation *compressed,
								 const int32 *values,
								 const uint64 *nonnull_rows, int nrows)
{
	PgBatchCompressedBatch *batch;
	PgBatchCompressedGroup *group;

	if (compressed->nbatches == compressed->batch_capacity)
	{
		compressed->batch_capacity *= 2;
		compressed->batches = repalloc_array(compressed->batches,
											 PgBatchCompressedBatch *,
											 compressed->batch_capacity);
	}
	batch = pg_batch_build_compressed_batch(values, nonnull_rows,
											compressed->ncolumns, nrows);
	if (compressed->ngroups == 0 ||
		compressed->groups[compressed->ngroups - 1]->nbatches ==
		compressed->group_batches)
		group = pg_batch_append_group(compressed);
	else
		group = compressed->groups[compressed->ngroups - 1];
	compressed->batches[compressed->nbatches++] = batch;
	compressed->bytes += batch->bytes;
	pg_batch_update_group(group, values, nonnull_rows,
						  compressed->ncolumns, nrows);
}

static void
pg_batch_install_compressed_relation(PgBatchCompressedRelation *compressed)
{
	PgBatchCompressedRelation **link = &compressed_relations;

	while (*link != NULL && (*link)->relid != compressed->relid)
		link = &(*link)->next;
	if (*link != NULL && (*link)->refcount != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("cannot replace pg_batch compressed snapshot while it is in use")));

	/* The snapshot outlives the SQL statement that built it. */
	MemoryContextSetParent(compressed->context, TopMemoryContext);
	if (*link != NULL)
	{
		PgBatchCompressedRelation *old = *link;

		compressed->next = old->next;
		*link = compressed;
		MemoryContextDelete(old->context);
	}
	else
	{
		compressed->next = compressed_relations;
		compressed_relations = compressed;
	}
}

Datum
pg_batch_compress(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		group_rows = PG_NARGS() > 1 ? PG_GETARG_INT32(1) :
		PG_BATCH_DEFAULT_GROUP_ROWS;
	Relation	relation = table_open(relid, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(relation);
	MemoryContext context;
	MemoryContext oldcontext;
	PgBatchCompressedRelation *compressed;
	TableScanDesc scan;
	TupleTableSlot *slot;
	int32	   *values;
	uint64	   *nonnull_rows;
	int			batch_row = 0;

	pg_batch_compressed_relation_supported(relation, true);
	if (group_rows < PG_BATCH_SIZE || group_rows % PG_BATCH_SIZE != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("storage group size must be a positive multiple of %d",
						PG_BATCH_SIZE)));
	context = AllocSetContextCreate(CurrentMemoryContext,
									"pg_batch compressed snapshot",
									ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(context);
	compressed = palloc0_object(PgBatchCompressedRelation);
	compressed->context = context;
	compressed->relid = relid;
	compressed->ncolumns = desc->natts;
	compressed->group_batches = group_rows / PG_BATCH_SIZE;
	compressed->batches = palloc_array(PgBatchCompressedBatch *, 32);
	compressed->batch_capacity = 32;
	compressed->groups = palloc_array(PgBatchCompressedGroup *, 8);
	compressed->group_capacity = 8;
	values = palloc0_array(int32, desc->natts * PG_BATCH_SIZE);
	nonnull_rows = palloc0_array(uint64, desc->natts);
	MemoryContextSwitchTo(oldcontext);

	scan = table_beginscan(relation, GetActiveSnapshot(), 0, NULL, 0);
	slot = table_slot_create(relation, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		slot_getallattrs(slot);
		for (int column = 0; column < desc->natts; column++)
		{
			int32		value = 0;

			if (!slot->tts_isnull[column])
			{
				value = DatumGetInt32(slot->tts_values[column]);
				nonnull_rows[column] |= UINT64CONST(1) << batch_row;
			}
			values[column * PG_BATCH_SIZE + batch_row] = value;
		}
		batch_row++;
		if (batch_row == PG_BATCH_SIZE)
		{
			oldcontext = MemoryContextSwitchTo(context);
			pg_batch_append_compressed_batch(compressed, values,
											 nonnull_rows, batch_row);
			MemoryContextSwitchTo(oldcontext);
			memset(nonnull_rows, 0, sizeof(uint64) * desc->natts);
			batch_row = 0;
		}
		ExecClearTuple(slot);
	}
	if (batch_row > 0)
	{
		oldcontext = MemoryContextSwitchTo(context);
		pg_batch_append_compressed_batch(compressed, values, nonnull_rows,
										 batch_row);
		MemoryContextSwitchTo(oldcontext);
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	table_close(relation, AccessShareLock);

	pfree(values);
	pfree(nonnull_rows);
	pg_batch_install_compressed_relation(compressed);
	PG_RETURN_INT64((int64) compressed->bytes);
}

static PgBatchCompressedRelation *
pg_batch_compressed_lookup(Relation relation)
{
	PgBatchCompressedRelation *compressed;

	if (!pg_batch_use_compressed ||
		!pg_batch_compressed_relation_supported(relation, false))
		return NULL;
	for (compressed = compressed_relations;
		 compressed != NULL; compressed = compressed->next)
	{
		if (compressed->relid == RelationGetRelid(relation) &&
			compressed->ncolumns == RelationGetNumberOfAttributes(relation))
		{
			compressed->refcount++;
			return compressed;
		}
	}
	return NULL;
}

bool
pg_batch_compressed_available(Relation relation)
{
	PgBatchCompressedRelation *compressed;

	if (!pg_batch_use_compressed ||
		!pg_batch_compressed_relation_supported(relation, false))
		return false;
	for (compressed = compressed_relations;
		 compressed != NULL; compressed = compressed->next)
	{
		if (compressed->relid == RelationGetRelid(relation) &&
			compressed->ncolumns == RelationGetNumberOfAttributes(relation))
			return true;
	}
	return false;
}

static void
pg_batch_compressed_release(PgBatchCompressedRelation *compressed)
{
	Assert(compressed->refcount > 0);
	compressed->refcount--;
}

typedef enum PgBatchGroupMatch
{
	PG_BATCH_GROUP_MATCH,
	PG_BATCH_GROUP_SKIP_MINMAX,
	PG_BATCH_GROUP_SKIP_MEMBERSHIP
} PgBatchGroupMatch;

static bool
pg_batch_group_contains(const PgBatchGroupColumn *column, int32 value)
{
	for (int i = 0; i < column->ndistinct; i++)
	{
		if (column->distinct[i] == value)
			return true;
	}
	return false;
}

static PgBatchGroupMatch
pg_batch_group_may_match(PgBatchSlot *bslot,
						 PgBatchCompressedGroup *group)
{
	for (int q = 0; q < bslot->request.nsource_quals; q++)
	{
		const PgBatchSourceQual *qual = &bslot->request.source_quals[q];
		AttrNumber	attnum;
		PgBatchGroupColumn *column;
		int32		scalar;
		bool		matches;

		if (qual->op == PG_BATCH_SOURCE_UNSUPPORTED)
			continue;
		if (qual->scalar.isnull)
			return PG_BATCH_GROUP_SKIP_MINMAX;
		if (qual->column < 0 || qual->column >= bslot->ncolumns)
			elog(ERROR, "pg_batch source restriction column is out of range");
		attnum = bslot->source_attnums[qual->column];
		column = &group->columns[attnum - 1];
		if (!column->has_nonnull)
			return PG_BATCH_GROUP_SKIP_MINMAX;
		scalar = DatumGetInt32(qual->scalar.value);
		switch (qual->op)
		{
			case PG_BATCH_SOURCE_EQ:
				matches = scalar >= column->minimum && scalar <= column->maximum;
				break;
			case PG_BATCH_SOURCE_NE:
				matches = column->minimum != scalar || column->maximum != scalar;
				break;
			case PG_BATCH_SOURCE_LT:
				matches = column->minimum < scalar;
				break;
			case PG_BATCH_SOURCE_LE:
				matches = column->minimum <= scalar;
				break;
			case PG_BATCH_SOURCE_GT:
				matches = column->maximum > scalar;
				break;
			case PG_BATCH_SOURCE_GE:
				matches = column->maximum >= scalar;
				break;
			case PG_BATCH_SOURCE_UNSUPPORTED:
				pg_unreachable();
		}
		if (!matches)
			return PG_BATCH_GROUP_SKIP_MINMAX;
		if (qual->op == PG_BATCH_SOURCE_EQ &&
			!column->distinct_overflow &&
			!pg_batch_group_contains(column, scalar))
			return PG_BATCH_GROUP_SKIP_MEMBERSHIP;
	}
	return PG_BATCH_GROUP_MATCH;
}

static void
pg_batch_compressed_prepare_columns(PgBatchSlot *bslot,
									const Bitmapset *columns, uint64 rows,
									PgBatchMaterializePhase phase)
{
	PgBatchCompressedActive *active = bslot->format_private;
	int			column = -1;

	if (columns == NULL || rows == 0)
		return;
	while ((column = bms_next_member(columns, column)) >= 0)
	{
		AttrNumber	source_attnum;
		PgBatchCompressedColumn *stored;
		PgBatchArrowColumn *arrow;
		const char *data;

		if (column >= bslot->ncolumns)
			elog(ERROR, "pg_batch column %d is out of range", column + 1);
		arrow = &active->columns[column];
		if (arrow->array.release != NULL)
			continue;
		source_attnum = bslot->source_attnums[column];
		stored = &active->batch->columns[source_attnum - 1];
		data = (const char *) active->batch + stored->data_offset;

		arrow->array.length = bslot->nrows;
		arrow->array.null_count = stored->null_count;
		arrow->array.offset = 0;
		arrow->array.n_buffers = 2;
		arrow->array.buffers = arrow->buffers;
		arrow->array.release = pg_batch_arrow_array_release;
		arrow->buffers[0] = stored->null_count == 0 ? NULL : stored->validity;
		if (stored->encoding == PG_BATCH_COMPRESS_PLAIN)
			arrow->buffers[1] = data;
		else
		{
			int32	   *decoded = MemoryContextAlloc(bslot->batch_context,
													 sizeof(int32) * bslot->nrows);

			decoded[0] = stored->base;
			for (int row = 1; row < bslot->nrows; row++)
			{
				int32		delta;

				if (stored->encoding == PG_BATCH_COMPRESS_DELTA8)
					delta = ((const int8 *) data)[row - 1];
				else
					delta = ((const int16 *) data)[row - 1];
				decoded[row] = decoded[row - 1] + delta;
			}
			arrow->buffers[1] = decoded;
			bslot->arrow_decoded_values += bslot->nrows;
		}
		arrow->schema.format = "i";
		arrow->schema.flags = ARROW_FLAG_NULLABLE;
		arrow->schema.release = pg_batch_arrow_schema_release;
		if (phase == PG_BATCH_FILTER_PHASE)
			bslot->arrow_filter_columns++;
		else
			bslot->arrow_project_columns++;
	}
}

static PgBatchArrowView
pg_batch_compressed_get_arrow_column(PgBatchSlot *bslot, int column)
{
	PgBatchCompressedActive *active = bslot->format_private;
	PgBatchArrowColumn *arrow;
	PgBatchArrowView view;

	if (column < 0 || column >= bslot->ncolumns)
		elog(ERROR, "pg_batch column %d is out of range", column + 1);
	arrow = &active->columns[column];
	if (arrow->array.release == NULL)
		elog(ERROR, "pg_batch column %d has not been prepared", column + 1);
	view.array = &arrow->array;
	view.schema = &arrow->schema;
	return view;
}

static void
pg_batch_compressed_clear_batch(PgBatchSlot *bslot)
{
	PgBatchCompressedActive *active = bslot->format_private;

	if (active == NULL)
		return;
	for (int column = 0; column < bslot->ncolumns; column++)
	{
		PgBatchArrowColumn *arrow = &active->columns[column];

		if (arrow->array.release != NULL)
			arrow->array.release(&arrow->array);
		if (arrow->schema.release != NULL)
			arrow->schema.release(&arrow->schema);
	}
}

static const PgBatchFormatOps pg_batch_compressed_format_ops = {
	.prepare_columns = pg_batch_compressed_prepare_columns,
	.get_arrow_column = pg_batch_compressed_get_arrow_column,
	.clear_batch = pg_batch_compressed_clear_batch,
};

static void
pg_batch_compressed_load_batch(PgBatchSlot *bslot,
							   PgBatchCompressedRelation *compressed,
							   PgBatchCompressedBatch *batch)
{
	TupleTableSlot *slot = &bslot->base;
	PgBatchCompressedActive *active;

	ExecClearTuple(slot);
	active = MemoryContextAllocZero(bslot->batch_context,
									offsetof(PgBatchCompressedActive, columns) +
									sizeof(PgBatchArrowColumn) * bslot->ncolumns);
	active->batch = batch;
	bslot->format_ops = &pg_batch_compressed_format_ops;
	bslot->format_private = active;
	bslot->buffer = InvalidBuffer;
	bslot->block = InvalidBlockNumber;
	bslot->table_oid = compressed->relid;
	bslot->nrows = batch->nrows;
	bslot->selected_rows = pg_batch_nrows_mask(batch->nrows);
	bslot->current_row = 0;
	bslot->batch_consumed = false;
	bslot->columns = MemoryContextAllocZero(bslot->batch_context,
											sizeof(PgBatchColumn) * bslot->ncolumns);
	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	pg_batch_slot_select_row(bslot, 0);
}

static bool
pg_batch_source_compare(PgBatchSourceOp op, int32 value, int32 scalar)
{
	switch (op)
	{
		case PG_BATCH_SOURCE_EQ:
			return value == scalar;
		case PG_BATCH_SOURCE_NE:
			return value != scalar;
		case PG_BATCH_SOURCE_LT:
			return value < scalar;
		case PG_BATCH_SOURCE_LE:
			return value <= scalar;
		case PG_BATCH_SOURCE_GT:
			return value > scalar;
		case PG_BATCH_SOURCE_GE:
			return value >= scalar;
		case PG_BATCH_SOURCE_UNSUPPORTED:
			break;
	}
	pg_unreachable();
}

static void
pg_batch_compressed_filter(PgBatchSlot *bslot,
						  PgBatchCompressedScan *scan)
{
	Bitmapset  *columns = NULL;
	uint64		initial = bslot->selected_rows;

	for (int q = 0; q < bslot->request.nsource_quals; q++)
	{
		const PgBatchSourceQual *qual = &bslot->request.source_quals[q];

		if (qual->op != PG_BATCH_SOURCE_UNSUPPORTED)
			columns = bms_add_member(columns, qual->column);
	}
	pg_batch_prepare_columns(bslot, columns, bslot->selected_rows,
							 PG_BATCH_FILTER_PHASE);
	for (int q = 0;
		 q < bslot->request.nsource_quals && bslot->selected_rows != 0; q++)
	{
		const PgBatchSourceQual *qual = &bslot->request.source_quals[q];
		PgBatchArrowView view;
		const int32 *values;
		uint64		rows;

		if (qual->op == PG_BATCH_SOURCE_UNSUPPORTED)
			continue;
		if (qual->scalar.isnull)
		{
			bslot->selected_rows = 0;
			break;
		}
		view = pg_batch_get_arrow_column(bslot, qual->column);
		values = view.array->buffers[1];
		rows = bslot->selected_rows;
		while (rows != 0)
		{
			int			row = pg_rightmost_one_pos64(rows);
			uint64		bit = UINT64CONST(1) << row;

			if (!pg_batch_arrow_row_is_valid(view.array, row) ||
				!pg_batch_source_compare(qual->op,
								 values[view.array->offset + row],
								 DatumGetInt32(qual->scalar.value)))
				bslot->selected_rows &= ~bit;
			rows &= ~bit;
		}
	}
	bms_free(columns);
	scan->stats.rows_removed_by_source_filter +=
		pg_batch_row_count(initial) - pg_batch_row_count(bslot->selected_rows);
}

static PgBatchCompressedScan *
pg_batch_compressed_scan_begin(PgBatchSlot *bslot, Relation relation)
{
	MemoryContext oldcontext;
	PgBatchCompressedScan *scan;

	oldcontext = MemoryContextSwitchTo(bslot->base.tts_mcxt);
	scan = palloc0_object(PgBatchCompressedScan);
	scan->relation = pg_batch_compressed_lookup(relation);
	MemoryContextSwitchTo(oldcontext);
	if (scan->relation == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_batch compressed snapshot is not available for relation \"%s\"",
						RelationGetRelationName(relation))));
	bslot->compressed_scan = scan;
	return scan;
}

bool
pg_batch_compressed_scan_next(PgBatchSlot *bslot, Relation relation)
{
	PgBatchCompressedScan *scan = bslot->compressed_scan;
	PgBatchCompressedRelation *compressed;

	if (scan == NULL)
		scan = pg_batch_compressed_scan_begin(bslot, relation);
	compressed = scan->relation;
	/*
	 * Storage groups decide whether any of their 64-row executor batches need
	 * to be touched. The active batch format and its lazy Arrow columns remain
	 * unchanged below this boundary.
	 */
	while (scan->group_index < compressed->ngroups)
	{
		PgBatchCompressedGroup *group =
			compressed->groups[scan->group_index];
		int			end_batch = group->first_batch + group->nbatches;

		if (!scan->group_ready)
		{
			PgBatchGroupMatch match = PG_BATCH_GROUP_MATCH;

			if (bslot->request.source_mode != PG_BATCH_COMPRESSED_BATCH)
			{
				scan->stats.groups_examined++;
				match = pg_batch_group_may_match(bslot, group);
			}
			scan->group_ready = true;
			if (match != PG_BATCH_GROUP_MATCH)
			{
				if (match == PG_BATCH_GROUP_SKIP_MINMAX)
					scan->stats.groups_skipped_minmax++;
				else
					scan->stats.groups_skipped_membership++;
				scan->batch_index = end_batch;
			}
		}
		if (scan->batch_index < end_batch)
		{
			PgBatchCompressedBatch *batch =
				compressed->batches[scan->batch_index++];

			pg_batch_compressed_load_batch(bslot, compressed, batch);
			scan->stats.encoded_bytes_touched += batch->bytes;
			if (bslot->request.source_mode == PG_BATCH_COMPRESSED_FILTER)
				pg_batch_compressed_filter(bslot, scan);
			return true;
		}
		scan->group_index++;
		scan->group_ready = false;
	}
	return false;
}

void
pg_batch_compressed_scan_rescan(PgBatchSlot *bslot)
{
	PgBatchCompressedScan *scan = bslot->compressed_scan;

	if (scan == NULL)
		return;
	scan->group_index = 0;
	scan->batch_index = 0;
	scan->group_ready = false;
}

void
pg_batch_compressed_scan_end(PgBatchSlot *bslot)
{
	PgBatchCompressedScan *scan = bslot->compressed_scan;

	if (scan == NULL)
		return;
	pg_batch_compressed_release(scan->relation);
	pfree(scan);
	bslot->compressed_scan = NULL;
}

const PgBatchCompressedStats *
pg_batch_compressed_scan_stats(PgBatchSlot *bslot)
{
	PgBatchCompressedScan *scan = bslot->compressed_scan;

	return scan == NULL ? NULL : &scan->stats;
}

void
pg_batch_compress_fini(void)
{
	PgBatchCompressedRelation *compressed = compressed_relations;

	while (compressed != NULL)
	{
		PgBatchCompressedRelation *next = compressed->next;

		MemoryContextDelete(compressed->context);
		compressed = next;
	}
	compressed_relations = NULL;
}
