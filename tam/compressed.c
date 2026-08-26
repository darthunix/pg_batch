#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_class.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "internal.h"

PG_FUNCTION_INFO_V1(pg_batch_compress);

#define PG_BATCH_DEFAULT_GROUP_ROWS 4096
#define PG_BATCH_DISTINCT_VALUES 16

typedef enum PgBatchCompression
{
	PG_BATCH_COMPRESS_PLAIN,
	PG_BATCH_COMPRESS_DELTA8,
	PG_BATCH_COMPRESS_DELTA16
} PgBatchCompression;

typedef struct CompressedColumn
{
	PgBatchCompression encoding;
	uint8		validity[PG_BATCH_SIZE / 8];
	uint16		null_count;
	int32		base;
	uint32		data_offset;
} CompressedColumn;

typedef struct CompressedBatch
{
	int			nrows;
	Size		bytes;
	CompressedColumn columns[FLEXIBLE_ARRAY_MEMBER];
} CompressedBatch;

typedef struct GroupColumn
{
	/* Coarse metadata belongs to a storage group, not a 64-row batch. */
	bool		has_nonnull;
	/* distinct[] is exact until more than this small fixed set is seen. */
	bool		distinct_overflow;
	uint8		ndistinct;
	int32		minimum;
	int32		maximum;
	int32		distinct[PG_BATCH_DISTINCT_VALUES];
} GroupColumn;

typedef struct StorageGroup
{
	int			first_batch;
	int			nbatches;
	GroupColumn columns[FLEXIBLE_ARRAY_MEMBER];
} StorageGroup;

typedef struct CompressedRelation CompressedRelation;

struct CompressedRelation
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
	CompressedBatch **batches;
	StorageGroup **groups;
	CompressedRelation *next;
};

typedef struct ArrowColumn
{
	struct ArrowArray array;
	struct ArrowSchema schema;
	const void *buffers[2];
} ArrowColumn;

typedef struct DatumColumn
{
	Datum	   *values;
	bool	   *isnull;
	uint64		valid_rows;
} DatumColumn;

struct ActiveBatch
{
	PgBatchBridgeBatch bridge_batch;
	CompressedScan *scan;
	CompressedBatch *batch;
	uint64		selection;
	ArrowColumn *arrow_columns;
	DatumColumn *datum_columns;
};

static CompressedRelation *compressed_relations;

static inline void
arrow_set_valid(uint8 *validity, int row)
{
	validity[row / 8] |= (uint8) 1 << (row % 8);
}

static void
arrow_array_release(struct ArrowArray *array)
{
	array->release = NULL;
}

static void
arrow_schema_release(struct ArrowSchema *schema)
{
	schema->release = NULL;
}

static bool
relation_supported(Relation relation, bool report_error)
{
	TupleDesc	desc = RelationGetDescr(relation);

	if (relation->rd_rel->relkind != RELKIND_RELATION ||
		!pg_batch_relation_uses_tableam(relation))
	{
		if (report_error)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("pg_batch_compress supports only pg_batch_compressed tables")));
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
choose_compression(const int32 *values, uint64 nonnull_rows,
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
write_column(CompressedBatch *batch, int column,
			 const int32 *values, uint64 nonnull_rows, int nrows)
{
	CompressedColumn *stored = &batch->columns[column];
	char	   *output = (char *) batch + stored->data_offset;
	int32		last = 0;

	for (int row = 0; row < nrows; row++)
	{
		if ((nonnull_rows & (UINT64CONST(1) << row)) != 0)
			arrow_set_valid(stored->validity, row);
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

static CompressedBatch *
build_compressed_batch(const int32 *values,
					   const uint64 *nonnull_rows,
					   int ncolumns, int nrows)
{
	PgBatchCompression *encodings = palloc_array(PgBatchCompression, ncolumns);
	Size	   *data_sizes = palloc_array(Size, ncolumns);
	int32	   *bases = palloc_array(int32, ncolumns);
	Size		header_size = offsetof(CompressedBatch, columns) +
		sizeof(CompressedColumn) * ncolumns;
	Size		bytes = MAXALIGN(header_size);
	CompressedBatch *batch;

	for (int column = 0; column < ncolumns; column++)
	{
		encodings[column] =
			choose_compression(values + column * PG_BATCH_SIZE,
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
		CompressedColumn *stored = &batch->columns[column];

		stored->encoding = encodings[column];
		stored->base = bases[column];
		stored->data_offset = header_size;
		write_column(batch, column,
					 values + column * PG_BATCH_SIZE,
					 nonnull_rows[column], nrows);
		header_size += MAXALIGN(data_sizes[column]);
	}
	pfree(encodings);
	pfree(data_sizes);
	pfree(bases);
	return batch;
}

static StorageGroup *
append_group(CompressedRelation *compressed)
{
	Size		bytes = offsetof(StorageGroup, columns) +
		sizeof(GroupColumn) * compressed->ncolumns;
	StorageGroup *group;

	if (compressed->ngroups == compressed->group_capacity)
	{
		compressed->group_capacity *= 2;
		compressed->groups = repalloc_array(compressed->groups,
											StorageGroup *,
											compressed->group_capacity);
	}
	group = palloc0(bytes);
	group->first_batch = compressed->nbatches;
	compressed->groups[compressed->ngroups++] = group;
	compressed->bytes += bytes;
	return group;
}

static void
update_group(StorageGroup *group, const int32 *values,
			 const uint64 *nonnull_rows, int ncolumns, int nrows)
{
	for (int column = 0; column < ncolumns; column++)
	{
		GroupColumn *meta = &group->columns[column];

		for (int row = 0; row < nrows; row++)
		{
			uint64		bit = UINT64CONST(1) << row;
			int32		value;
			bool		known = false;

			if ((nonnull_rows[column] & bit) == 0)
				continue;
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
append_compressed_batch(CompressedRelation *compressed,
						const int32 *values,
						const uint64 *nonnull_rows, int nrows)
{
	CompressedBatch *batch;
	StorageGroup *group;

	if (compressed->nbatches == compressed->batch_capacity)
	{
		compressed->batch_capacity *= 2;
		compressed->batches = repalloc_array(compressed->batches,
											 CompressedBatch *,
											 compressed->batch_capacity);
	}
	batch = build_compressed_batch(values, nonnull_rows,
								   compressed->ncolumns, nrows);
	if (compressed->ngroups == 0 ||
		compressed->groups[compressed->ngroups - 1]->nbatches ==
		compressed->group_batches)
		group = append_group(compressed);
	else
		group = compressed->groups[compressed->ngroups - 1];
	compressed->batches[compressed->nbatches++] = batch;
	compressed->bytes += batch->bytes;
	update_group(group, values, nonnull_rows,
				 compressed->ncolumns, nrows);
}

static void
install_compressed_relation(CompressedRelation *compressed)
{
	CompressedRelation **link = &compressed_relations;

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
		CompressedRelation *old = *link;

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
	CompressedRelation *compressed;
	TableScanDesc scan;
	TupleTableSlot *slot;
	int32	   *values;
	uint64	   *nonnull_rows;
	int			batch_row = 0;

	relation_supported(relation, true);
	if (group_rows < PG_BATCH_SIZE || group_rows % PG_BATCH_SIZE != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("storage group size must be a positive multiple of %d",
						PG_BATCH_SIZE)));
	context = AllocSetContextCreate(CurrentMemoryContext,
									"pg_batch compressed snapshot",
									ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(context);
	compressed = palloc0_object(CompressedRelation);
	compressed->context = context;
	compressed->relid = relid;
	compressed->ncolumns = desc->natts;
	compressed->group_batches = group_rows / PG_BATCH_SIZE;
	compressed->batches = palloc_array(CompressedBatch *, 32);
	compressed->batch_capacity = 32;
	compressed->groups = palloc_array(StorageGroup *, 8);
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
			append_compressed_batch(compressed, values,
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
		append_compressed_batch(compressed, values, nonnull_rows,
								batch_row);
		MemoryContextSwitchTo(oldcontext);
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	table_close(relation, AccessShareLock);

	pfree(values);
	pfree(nonnull_rows);
	install_compressed_relation(compressed);
	PG_RETURN_INT64((int64) compressed->bytes);
}

static CompressedRelation *
find_compressed_relation(Relation relation)
{
	CompressedRelation *compressed;

	if (!relation_supported(relation, false))
		return NULL;
	for (compressed = compressed_relations;
		 compressed != NULL; compressed = compressed->next)
	{
		if (compressed->relid == RelationGetRelid(relation) &&
			compressed->ncolumns == RelationGetNumberOfAttributes(relation))
			return compressed;
	}
	return NULL;
}

CompressedRelation *
pg_batch_compressed_acquire(Relation relation)
{
	CompressedRelation *compressed = find_compressed_relation(relation);

	if (compressed != NULL)
		compressed->refcount++;
	return compressed;
}

bool
pg_batch_compressed_available(Relation relation)
{
	return find_compressed_relation(relation) != NULL;
}

void
pg_batch_compressed_release(CompressedRelation *compressed)
{
	Assert(compressed->refcount > 0);
	compressed->refcount--;
}

typedef enum GroupMatch
{
	PG_BATCH_GROUP_MATCH,
	PG_BATCH_GROUP_SKIP_MINMAX,
	PG_BATCH_GROUP_SKIP_MEMBERSHIP
} GroupMatch;

static bool
group_contains(const GroupColumn *column, int32 value)
{
	for (int i = 0; i < column->ndistinct; i++)
	{
		if (column->distinct[i] == value)
			return true;
	}
	return false;
}

static GroupMatch
group_may_match(CompressedScan *scan,
				StorageGroup *group)
{
	for (int q = 0; q < scan->nquals; q++)
	{
		const SourceQual *qual = &scan->quals[q];
		GroupColumn *column;
		int32		scalar;
		bool		matches;

		if (qual->scalar.isnull)
			return PG_BATCH_GROUP_SKIP_MINMAX;
		column = &group->columns[qual->attnum - 1];
		if (!column->has_nonnull)
			return PG_BATCH_GROUP_SKIP_MINMAX;
		scalar = DatumGetInt32(qual->scalar.value);
		switch (qual->op)
		{
			case PG_BATCH_TAM_SOURCE_EQ:
				matches = scalar >= column->minimum && scalar <= column->maximum;
				break;
			case PG_BATCH_TAM_SOURCE_NE:
				matches = column->minimum != scalar || column->maximum != scalar;
				break;
			case PG_BATCH_TAM_SOURCE_LT:
				matches = column->minimum < scalar;
				break;
			case PG_BATCH_TAM_SOURCE_LE:
				matches = column->minimum <= scalar;
				break;
			case PG_BATCH_TAM_SOURCE_GT:
				matches = column->maximum > scalar;
				break;
			case PG_BATCH_TAM_SOURCE_GE:
				matches = column->maximum >= scalar;
				break;
		}
		if (!matches)
			return PG_BATCH_GROUP_SKIP_MINMAX;
		if (qual->op == PG_BATCH_TAM_SOURCE_EQ &&
			!column->distinct_overflow &&
			!group_contains(column, scalar))
			return PG_BATCH_GROUP_SKIP_MEMBERSHIP;
	}
	return PG_BATCH_GROUP_MATCH;
}

static const int32 *
column_values(ActiveBatch *active, CompressedColumn *stored)
{
	CompressedScan *scan = active->scan;
	const char *data = (const char *) active->batch + stored->data_offset;
	int32	   *decoded;

	if (stored->encoding == PG_BATCH_COMPRESS_PLAIN)
		return (const int32 *) data;
	decoded = MemoryContextAlloc(scan->batch_context,
								 sizeof(int32) * active->bridge_batch.nrows);
	decoded[0] = stored->base;
	for (int row = 1; row < active->bridge_batch.nrows; row++)
	{
		int32		delta = stored->encoding == PG_BATCH_COMPRESS_DELTA8 ?
			((const int8 *) data)[row - 1] :
			((const int16 *) data)[row - 1];

		decoded[row] = decoded[row - 1] + delta;
	}
	scan->stats.arrow_decoded_values += active->bridge_batch.nrows;
	return decoded;
}

static pg_always_inline void
prepare_column(ActiveBatch *active, int column,
			   PgBatchBridgeMaterializePhase phase)
{
	CompressedScan *scan = active->scan;
	PgBatchBridgeBatch *bridge_batch = &active->bridge_batch;
	AttrNumber	source_attnum;
	CompressedColumn *stored;
	ArrowColumn *arrow;

	if (column < 0 || column >= scan->request->ncolumns)
		elog(ERROR, "pg_batch column %d is out of range", column + 1);
	arrow = &active->arrow_columns[column];
	if (arrow->array.release != NULL)
		return;
	source_attnum = scan->request->source_attnums[column];
	stored = &active->batch->columns[source_attnum - 1];
	arrow->array.length = bridge_batch->nrows;
	arrow->array.null_count = stored->null_count;
	arrow->array.offset = 0;
	arrow->array.n_buffers = 2;
	arrow->array.buffers = arrow->buffers;
	arrow->array.release = arrow_array_release;
	arrow->buffers[0] = stored->null_count == 0 ? NULL : stored->validity;
	arrow->buffers[1] = column_values(active, stored);
	arrow->schema.format = "i";
	arrow->schema.flags = ARROW_FLAG_NULLABLE;
	arrow->schema.release = arrow_schema_release;
	if (phase == PG_BATCH_BRIDGE_MATERIALIZE_FILTER)
		scan->stats.arrow_filter_columns++;
	else
		scan->stats.arrow_project_columns++;
}

static void
prepare_columns(PgBatchBridgeBatch *bridge_batch,
				const Bitmapset *columns,
				const uint64 *rows,
				PgBatchBridgeMaterializePhase phase)
{
	ActiveBatch *active = bridge_batch->private_data;
	int			column = -1;

	Assert(bridge_batch->nwords == 1);
	if (columns == NULL || rows[0] == 0)
		return;
	while ((column = bms_next_member(columns, column)) >= 0)
		prepare_column(active, column, phase);
}

static void
get_arrow_column(PgBatchBridgeBatch *bridge_batch,
				 int column,
				 PgBatchBridgeArrowView *view)
{
	ActiveBatch *active = bridge_batch->private_data;
	CompressedScan *scan = active->scan;
	ArrowColumn *arrow;

	if (column < 0 || column >= scan->request->ncolumns)
		elog(ERROR, "pg_batch column %d is out of range", column + 1);
	arrow = &active->arrow_columns[column];
	if (arrow->array.release == NULL)
		elog(ERROR, "pg_batch column %d has not been prepared", column + 1);
	view->array = &arrow->array;
	view->schema = &arrow->schema;
}

static void
release_batch(PgBatchBridgeBatch *bridge_batch)
{
	ActiveBatch *active = bridge_batch->private_data;
	CompressedScan *scan;

	if (active == NULL)
		return;
	scan = active->scan;
	for (int column = 0; column < scan->request->ncolumns; column++)
	{
		ArrowColumn *arrow = &active->arrow_columns[column];

		if (arrow->array.release != NULL)
			arrow->array.release(&arrow->array);
		if (arrow->schema.release != NULL)
			arrow->schema.release(&arrow->schema);
	}
	scan->active = NULL;
}

static const PgBatchBridgeArrowInterface pg_batch_compressed_arrow = {
	.abi_version = PG_BATCH_BRIDGE_ARROW_INTERFACE_VERSION,
	.struct_size = sizeof(PgBatchBridgeArrowInterface),
	.get_column = get_arrow_column,
};

static void
get_datum_column(PgBatchBridgeBatch *bridge_batch,
				 int column, const uint64 *rows,
				 PgBatchBridgeMaterializePhase phase,
				 PgBatchBridgeDatumColumn *result)
{
	ActiveBatch *active = bridge_batch->private_data;
	CompressedScan *scan = active->scan;
	DatumColumn *datum;
	PgBatchBridgeArrowView view;
	const int32 *values;
	uint64		missing;

	if (column < 0 || column >= scan->request->ncolumns)
		elog(ERROR, "pg_batch column %d is out of range", column + 1);
	prepare_column(active, column, phase);
	get_arrow_column(bridge_batch, column, &view);
	datum = &active->datum_columns[column];
	if (datum->values == NULL)
	{
		datum->values = MemoryContextAlloc(scan->batch_context,
										   sizeof(Datum) * bridge_batch->nrows);
		datum->isnull = MemoryContextAlloc(scan->batch_context,
										   sizeof(bool) * bridge_batch->nrows);
	}
	values = view.array->buffers[1];
	missing = rows[0] & ~datum->valid_rows;
	while (missing != 0)
	{
		int			row = pg_rightmost_one_pos64(missing);
		uint64		bit = UINT64CONST(1) << row;
		bool		isnull = !pg_batch_arrow_row_is_valid(view.array, row);

		datum->values[row] = isnull ? (Datum) 0 :
			Int32GetDatum(values[view.array->offset + row]);
		datum->isnull[row] = isnull;
		datum->valid_rows |= bit;
		if (phase == PG_BATCH_BRIDGE_MATERIALIZE_FILTER)
			scan->stats.filter_datums++;
		else
			scan->stats.project_datums++;
		missing &= ~bit;
	}
	result->values = datum->values;
	result->isnull = datum->isnull;
	result->valid_rows = &datum->valid_rows;
	result->nwords = 1;
}

static const void *
get_native_interface(PgBatchBridgeBatch *batch,
					 const char *name, uint32 version)
{
	if (version == PG_BATCH_BRIDGE_ARROW_INTERFACE_VERSION &&
		strcmp(name, PG_BATCH_BRIDGE_ARROW_INTERFACE_NAME) == 0)
		return &pg_batch_compressed_arrow;
	return NULL;
}

static const PgBatchBridgeBatchOps pg_batch_compressed_batch_ops = {
	.abi_version = PG_BATCH_BRIDGE_ABI_VERSION,
	.struct_size = sizeof(PgBatchBridgeBatchOps),
	.prepare_columns = prepare_columns,
	.get_datum_column = get_datum_column,
	.get_native_interface = get_native_interface,
	.release = release_batch,
};

static void
load_batch(CompressedScan *scan,
		   CompressedBatch *batch)
{
	ActiveBatch *active;

	Assert(scan->active == NULL);
	MemoryContextReset(scan->batch_context);
	active = MemoryContextAllocZero(scan->batch_context, sizeof(*active));
	active->scan = scan;
	active->batch = batch;
	active->arrow_columns = MemoryContextAllocZero(scan->batch_context,
												   sizeof(ArrowColumn) *
												   scan->request->ncolumns);
	active->datum_columns = MemoryContextAllocZero(scan->batch_context,
												   sizeof(DatumColumn) *
												   scan->request->ncolumns);
	active->selection = PG_BATCH_ALL_ROWS >> (PG_BATCH_SIZE - batch->nrows);
	active->bridge_batch.abi_version = PG_BATCH_BRIDGE_ABI_VERSION;
	active->bridge_batch.struct_size = sizeof(PgBatchBridgeBatch);
	active->bridge_batch.nrows = batch->nrows;
	active->bridge_batch.nwords = 1;
	active->bridge_batch.selection = &active->selection;
	active->bridge_batch.table_oid = scan->relation->relid;
	active->bridge_batch.ops = &pg_batch_compressed_batch_ops;
	active->bridge_batch.private_data = active;
	scan->active = active;
}

static bool
compare_source(SourceOperator op, int32 value, int32 scalar)
{
	switch (op)
	{
		case PG_BATCH_TAM_SOURCE_EQ:
			return value == scalar;
		case PG_BATCH_TAM_SOURCE_NE:
			return value != scalar;
		case PG_BATCH_TAM_SOURCE_LT:
			return value < scalar;
		case PG_BATCH_TAM_SOURCE_LE:
			return value <= scalar;
		case PG_BATCH_TAM_SOURCE_GT:
			return value > scalar;
		case PG_BATCH_TAM_SOURCE_GE:
			return value >= scalar;
	}
	pg_unreachable();
}

static void
filter_batch(CompressedScan *scan)
{
	ActiveBatch *active = scan->active;
	uint64		initial = active->selection;

	for (int q = 0; q < scan->nquals && active->selection != 0; q++)
	{
		const SourceQual *qual = &scan->quals[q];
		CompressedColumn *stored =
			&active->batch->columns[qual->attnum - 1];
		const int32 *values;
		uint64		rows;

		if (qual->scalar.isnull)
		{
			active->selection = 0;
			break;
		}
		values = column_values(active, stored);
		rows = active->selection;
		while (rows != 0)
		{
			int			row = pg_rightmost_one_pos64(rows);
			uint64		bit = UINT64CONST(1) << row;

			if ((stored->validity[row / 8] &
				 ((uint8) 1 << (row % 8))) == 0 ||
				!compare_source(qual->op,
								values[row],
								DatumGetInt32(qual->scalar.value)))
				active->selection &= ~bit;
			rows &= ~bit;
		}
	}
	scan->stats.rows_removed_by_source_filter +=
		pg_popcount64(initial) - pg_popcount64(active->selection);
}

static void
prepare_source_quals(CompressedScan *scan)
{
	ResetExprContext(scan->econtext);
	for (int q = 0; q < scan->nquals; q++)
		scan->quals[q].scalar.value =
			ExecEvalExpr(scan->quals[q].scalar_expr, scan->econtext,
						 &scan->quals[q].scalar.isnull);
	scan->quals_ready = true;
}

bool
pg_batch_compressed_scan_next(PgBatchBridgeBinding *binding,
							  CompressedScan *scan)
{
	CompressedRelation *compressed = scan->relation;

	if (!scan->quals_ready)
		prepare_source_quals(scan);

	/*
	 * Storage groups decide whether any of their 64-row executor batches need
	 * to be touched. The active batch format and its lazy Arrow columns
	 * remain unchanged below this boundary.
	 */
	while (scan->group_index < compressed->ngroups)
	{
		StorageGroup *group =
			compressed->groups[scan->group_index];
		int			end_batch = group->first_batch + group->nbatches;

		if (!scan->group_ready)
		{
			GroupMatch	match = PG_BATCH_GROUP_MATCH;

			if (scan->mode != PG_BATCH_TAM_BATCH)
			{
				scan->stats.groups_examined++;
				match = group_may_match(scan, group);
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
			CompressedBatch *stored_batch =
				compressed->batches[scan->batch_index++];

			load_batch(scan, stored_batch);
			scan->stats.encoded_bytes_touched += stored_batch->bytes;
			if (scan->mode == PG_BATCH_TAM_FILTER)
				filter_batch(scan);
			if (scan->active->selection == 0)
			{
				release_batch(
							  &scan->active->bridge_batch);
				continue;
			}
			pg_batch_tam_bridge->publish_batch(
											   binding,
											   &scan->active->bridge_batch);
			return true;
		}
		scan->group_index++;
		scan->group_ready = false;
	}
	return false;
}

void
pg_batch_compressed_fini(void)
{
	CompressedRelation *compressed = compressed_relations;

	while (compressed != NULL)
	{
		CompressedRelation *next = compressed->next;

		MemoryContextDelete(compressed->context);
		compressed = next;
	}
	compressed_relations = NULL;
}
