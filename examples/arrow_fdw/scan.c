#include "postgres.h"

#include <stdio.h>

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "executor/nodeForeignscan.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "port/pg_bitutils.h"
#include "storage/fd.h"
#include "utils/memutils.h"

#include "internal.h"

/*
 * Keep one IPC record body alive and ask nanoarrow to decode individual child
 * arrays only when a filter or projection needs them. ActiveBatch is a
 * 64-row window over those record-wide arrays; its ArrowArray copies borrow
 * the reader's body and are invalidated before the next record is read.
 */
typedef struct IpcReader
{
	FILE	   *file;
	ArrowIpcDecoder decoder;
	ArrowSchema schema;
	ArrowBuffer header;
	ArrowBuffer body;
	bool		decoder_initialized;
	bool		schema_initialized;
} IpcReader;

typedef struct SourceQual
{
	PgBatchExpr *expr;
	Bitmapset  *column_mask;
	int			column;
} SourceQual;

typedef struct DatumColumn
{
	Datum	   *values;
	bool	   *isnull;
	uint64		valid_rows;
} DatumColumn;

typedef struct ActiveBatch
{
	struct PgBatchFdwScan *scan;
	PgBatch bridge_batch;
	uint64		selection;
	int			start;
	ArrowArray *columns;
	bool	   *prepared;
	DatumColumn *datums;
} ActiveBatch;

struct PgBatchFdwScan
{
	MemoryContextCallback cleanup;
	MemoryContext context;
	MemoryContext batch_context;
	Relation	relation;
	ExprContext *econtext;
	const PgBatchRequest *request;
	const AttrNumber *source_attnums;
	int			ncolumns;
	IpcReader	reader;
	ArrowArray *record_columns;
	bool	   *record_decoded;
	int			record_length;
	int			record_offset;
	SourceQual *quals;
	int			nquals;
	bool		cleaned;
	ActiveBatch *active;
	int			scalar_row;
	PgBatchFdwStats stats;
};

static const PgBatchOps batch_ops;
static void prepare_columns(PgBatch *bridge_batch,
	const Bitmapset *columns, const uint64 *rows,
	PgBatchColumnPhase phase);

static void
nanoarrow_error(int code, ArrowError *error, const char *operation)
{
	if (code == NANOARROW_OK)
		return;
	ereport(ERROR,
			(errcode(ERRCODE_FDW_ERROR),
			 errmsg("pg_batch_fdw could not %s", operation),
			 errdetail("nanoarrow error %d: %s", code,
					   error->message[0] == '\0' ? "no detail" : error->message)));
}

static int64
read_bytes(PgBatchFdwScan *scan, void *output, int64 size, bool allow_eof)
{
	char	   *position = output;
	int64		total = 0;

	while (total < size)
	{
		size_t		read_now = fread(position + total, 1, size - total,
								  scan->reader.file);

		if (read_now == 0)
		{
			if (ferror(scan->reader.file))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read pg_batch_fdw file: %m")));
			if (allow_eof && total == 0)
				return 0;
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("unexpected end of pg_batch_fdw Arrow stream")));
		}
		total += read_now;
	}
	scan->stats.bytes_read += total;
	return total;
}

static bool
read_header(PgBatchFdwScan *scan)
{
	IpcReader  *reader = &scan->reader;
	ArrowBufferView view;
	ArrowError	error;
	int32		prefix_size = 0;
	int			result;

	ArrowErrorInit(&error);
	reader->header.size_bytes = 0;
	nanoarrow_error(ArrowBufferReserve(&reader->header, 8), &error,
					"reserve an Arrow message header");
	if (read_bytes(scan, reader->header.data, 8, true) == 0)
		return false;
	reader->header.size_bytes = 8;
	view.data.data = reader->header.data;
	view.size_bytes = reader->header.size_bytes;
	result = ArrowIpcDecoderPeekHeader(&reader->decoder, view,
									   &prefix_size, &error);
	if (result == ENODATA)
		return false;
	nanoarrow_error(result, &error, "inspect an Arrow message header");
	if (prefix_size != 8)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_batch_fdw supports only modern Arrow IPC streams")));
	if (reader->decoder.header_size_bytes < 8)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid Arrow IPC message header size")));
	if (reader->decoder.header_size_bytes > 8)
	{
		int64		remainder = reader->decoder.header_size_bytes - 8;

		nanoarrow_error(ArrowBufferReserve(&reader->header, remainder), &error,
						"reserve an Arrow message header");
		read_bytes(scan, reader->header.data + 8, remainder, false);
		reader->header.size_bytes += remainder;
	}
	view.data.data = reader->header.data;
	view.size_bytes = reader->header.size_bytes;
	nanoarrow_error(ArrowIpcDecoderVerifyHeader(&reader->decoder, view, &error),
					&error, "verify an Arrow message header");
	nanoarrow_error(ArrowIpcDecoderDecodeHeader(&reader->decoder, view, &error),
					&error, "decode an Arrow message header");
	return true;
}

static void
read_body(PgBatchFdwScan *scan)
{
	IpcReader  *reader = &scan->reader;
	ArrowError	error;
	int64		size = reader->decoder.body_size_bytes;

	ArrowErrorInit(&error);
	reader->body.size_bytes = 0;
	nanoarrow_error(ArrowBufferReserve(&reader->body, size), &error,
					"reserve an Arrow record batch body");
	if (size > 0)
		read_bytes(scan, reader->body.data, size, false);
	reader->body.size_bytes = size;
}

static void
release_record(PgBatchFdwScan *scan)
{
	int			natts = RelationGetNumberOfAttributes(scan->relation);

	for (int i = 0; i < natts; i++)
	{
		if (scan->record_columns[i].release != NULL)
			ArrowArrayRelease(&scan->record_columns[i]);
		scan->record_decoded[i] = false;
	}
	scan->record_length = 0;
	scan->record_offset = 0;
}

static void
reader_init(PgBatchFdwScan *scan, const char *filename)
{
	IpcReader  *reader = &scan->reader;
	ArrowError	error;
	int			result;
	int			natts = RelationGetNumberOfAttributes(scan->relation);

	reader->file = AllocateFile(filename, "rb");
	if (reader->file == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open pg_batch_fdw file \"%s\": %m", filename)));
	ArrowBufferInit(&reader->header);
	ArrowBufferInit(&reader->body);
	result = ArrowIpcDecoderInit(&reader->decoder);
	ArrowErrorInit(&error);
	nanoarrow_error(result, &error, "initialize the Arrow decoder");
	reader->decoder_initialized = true;
	if (!read_header(scan) ||
		reader->decoder.message_type != NANOARROW_IPC_MESSAGE_TYPE_SCHEMA)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_batch_fdw file does not begin with an Arrow schema")));
	if (reader->decoder.feature_flags != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_batch_fdw supports only uncompressed Arrow streams without dictionaries")));
	nanoarrow_error(ArrowIpcDecoderSetEndianness(&reader->decoder,
											 reader->decoder.endianness),
					&error, "set Arrow stream endianness");
	reader->schema.release = NULL;
	nanoarrow_error(ArrowIpcDecoderDecodeSchema(&reader->decoder,
										&reader->schema, &error),
					&error, "decode the Arrow schema");
	reader->schema_initialized = true;
	if (reader->schema.n_children != natts)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("Arrow schema has %lld columns but foreign table has %d",
						(long long) reader->schema.n_children, natts)));
	for (int i = 0; i < natts; i++)
	{
		if (strcmp(reader->schema.children[i]->format, "i") != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("Arrow column %d is not int32", i + 1)));
	}
	nanoarrow_error(ArrowIpcDecoderSetSchema(&reader->decoder,
										 &reader->schema, &error),
					&error, "configure the Arrow record decoder");
}

static void
reader_reset(PgBatchFdwScan *scan)
{
	IpcReader  *reader = &scan->reader;
	char	   *filename = pg_batch_fdw_get_filename(RelationGetRelid(scan->relation));

	release_record(scan);
	if (reader->schema_initialized)
		ArrowSchemaRelease(&reader->schema);
	if (reader->decoder_initialized)
		ArrowIpcDecoderReset(&reader->decoder);
	ArrowBufferReset(&reader->header);
	ArrowBufferReset(&reader->body);
	if (reader->file != NULL)
		FreeFile(reader->file);
	MemSet(reader, 0, sizeof(*reader));
	reader_init(scan, filename);
	pfree(filename);
}

static bool
reader_next_record(PgBatchFdwScan *scan)
{
	IpcReader  *reader = &scan->reader;

	release_record(scan);
	if (!read_header(scan))
		return false;
	if (reader->decoder.message_type != NANOARROW_IPC_MESSAGE_TYPE_RECORD_BATCH)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_batch_fdw supports only schema and record batch IPC messages")));
	if (reader->decoder.codec != NANOARROW_IPC_COMPRESSION_TYPE_NONE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_batch_fdw supports only uncompressed Arrow record batches")));
	read_body(scan);
	scan->stats.record_batches++;
	return true;
}

static ArrowArray *
decode_column(PgBatchFdwScan *scan, AttrNumber attnum,
			  PgBatchColumnPhase phase)
{
	IpcReader  *reader = &scan->reader;
	ArrowArray *column;
	ArrowBufferView body;
	ArrowError	error;
	int			index = attnum - 1;

	if (attnum <= 0 || attnum > RelationGetNumberOfAttributes(scan->relation))
		elog(ERROR, "pg_batch_fdw column %d is out of range", attnum);
	column = &scan->record_columns[index];
	if (scan->record_decoded[index])
		return column;
	body.data.data = reader->body.data;
	body.size_bytes = reader->body.size_bytes;
	ArrowErrorInit(&error);
	column->release = NULL;
	nanoarrow_error(ArrowIpcDecoderDecodeArray(&reader->decoder, body, index,
										   column,
										   NANOARROW_VALIDATION_LEVEL_MINIMAL,
										   &error),
					&error, "decode an Arrow column");
	scan->record_decoded[index] = true;
	scan->stats.columns_decoded++;
	if (phase == PG_BATCH_COLUMN_FILTER)
		scan->stats.filter_columns_decoded++;
	else
		scan->stats.project_columns_decoded++;
	return column;
}

static PgBatchColumnPhase
request_phase_for_attnum(PgBatchFdwScan *scan, AttrNumber attnum)
{
	for (int i = 0; i < scan->nquals; i++)
	{
		int			column = scan->quals[i].column;

		if (scan->source_attnums[column] == attnum)
			return PG_BATCH_COLUMN_FILTER;
	}
	for (int column = 0; column < scan->ncolumns; column++)
	{
		if (scan->source_attnums[column] == attnum)
			return bms_is_member(column, scan->request->filter_columns) ?
				PG_BATCH_COLUMN_FILTER :
				PG_BATCH_COLUMN_PROJECT;
	}
	return PG_BATCH_COLUMN_PROJECT;
}

static bool
load_record(PgBatchFdwScan *scan)
{
	AttrNumber	length_attnum;
	ArrowArray *length_column;
	int			natts = RelationGetNumberOfAttributes(scan->relation);

	if (!reader_next_record(scan))
		return false;
	if (scan->nquals > 0)
		length_attnum = scan->source_attnums[
			scan->quals[0].column];
	else if (scan->ncolumns > 0)
		length_attnum = scan->source_attnums[0];
	else
		length_attnum = 1;
	length_column = decode_column(scan, length_attnum,
							  request_phase_for_attnum(scan, length_attnum));
	if (length_column->length > INT_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("Arrow record batch is too large")));
	scan->record_length = length_column->length;
	if (!pg_batch_fdw_column_pruning)
	{
		for (int i = 0; i < natts; i++)
			decode_column(scan, i + 1,
						  request_phase_for_attnum(scan, i + 1));
	}
	return true;
}

static void
apply_source_quals(ActiveBatch *active)
{
	PgBatchFdwScan *scan = active->scan;
	int			initial = pg_popcount64(active->selection);

	ResetExprContext(scan->econtext);
	for (int q = 0; q < scan->nquals && active->selection != 0; q++)
	{
		SourceQual *qual = &scan->quals[q];

		prepare_columns(&active->bridge_batch, qual->column_mask,
						&active->selection, PG_BATCH_COLUMN_FILTER);
		pg_batch_expr_bind(qual->expr, &active->bridge_batch, scan->econtext,
						   PG_BATCH_COLUMN_FILTER);
		pg_batch_expr_apply_filter(qual->expr, true);
	}
	scan->stats.rows_removed += initial - pg_popcount64(active->selection);
}

static int
resolve_source_var(const Var *var, void *context)
{
	const PgBatchFdwScan *scan = context;

	if (var->varno == INDEX_VAR && var->varattno <= scan->ncolumns)
		return var->varattno - 1;
	for (int column = 0; column < scan->ncolumns; column++)
	{
		if (scan->source_attnums[column] == var->varattno)
			return column;
	}
	return -1;
}

static void
prepare_column(ActiveBatch *active, int column,
			   PgBatchColumnPhase phase)
{
	PgBatchFdwScan *scan = active->scan;
	AttrNumber	attnum;
	ArrowArray *source;
	ArrowArray *window;

	if (column < 0 || column >= scan->ncolumns)
		elog(ERROR, "pg_batch_fdw compact column %d is out of range", column + 1);
	if (active->prepared[column])
		return;
	attnum = scan->source_attnums[column];
	source = decode_column(scan, attnum, phase);
	window = &active->columns[column];
	*window = *source;
	window->length = active->bridge_batch.nrows;
	window->offset = source->offset + active->start;
	window->release = NULL;
	window->private_data = NULL;
	active->prepared[column] = true;
}

static void
prepare_columns(PgBatch *bridge_batch,
				const Bitmapset *columns, const uint64 *rows,
				PgBatchColumnPhase phase)
{
	ActiveBatch *active = bridge_batch->private_data;
	int			column = -1;

	if (columns == NULL || rows[0] == 0)
		return;
	while ((column = bms_next_member(columns, column)) >= 0)
		prepare_column(active, column, phase);
}

static void
get_arrow_column(PgBatch *bridge_batch, int column,
				 PgBatchArrowView *view)
{
	ActiveBatch *active = bridge_batch->private_data;
	PgBatchFdwScan *scan = active->scan;
	AttrNumber	attnum;

	if (column < 0 || column >= scan->ncolumns ||
		!active->prepared[column])
		elog(ERROR, "pg_batch_fdw column %d has not been prepared", column + 1);
	attnum = scan->source_attnums[column];
	view->array = &active->columns[column];
	view->schema = scan->reader.schema.children[attnum - 1];
}

static const PgBatchArrowInterface arrow_interface = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_ARROW_INTERFACE_VERSION,
		PgBatchArrowInterface),
	.get_column = get_arrow_column,
};

static bool
get_int4_column(PgBatch *bridge_batch, int column,
				PgBatchInt4Vector *result)
{
	PgBatchArrowView view;

	get_arrow_column(bridge_batch, column, &view);
	pg_batch_int4_vector_init_packed(result, view.array->buffers[1],
		view.array->null_count == 0 ? NULL : view.array->buffers[0],
		view.array->offset);
	return true;
}

static const PgBatchInt4VectorInterface int4_interface = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_INT4_VECTOR_INTERFACE_VERSION,
		PgBatchInt4VectorInterface),
	.get_column = get_int4_column,
};

static const PgBatchNativeInterface *
get_native_interface(const PgBatchNativeType *type)
{
	if (type->abi_version == PG_BATCH_INT4_VECTOR_INTERFACE_VERSION &&
		strcmp(type->name, PG_BATCH_INT4_VECTOR_INTERFACE_NAME) == 0)
		return (const PgBatchNativeInterface *) &int4_interface;
	if (type->abi_version == PG_BATCH_ARROW_INTERFACE_VERSION &&
		strcmp(type->name, PG_BATCH_ARROW_INTERFACE_NAME) == 0)
		return (const PgBatchNativeInterface *) &arrow_interface;
	return NULL;
}

static void
get_datum_column(PgBatch *bridge_batch, int column,
				 const uint64 *rows,
				 PgBatchColumnPhase phase,
				 PgBatchDatumVector *result)
{
	ActiveBatch *active = bridge_batch->private_data;
	PgBatchFdwScan *scan = active->scan;
	DatumColumn *datum;
	ArrowArray *array;
	const int32 *values;
	uint64		missing;

	pg_batch_check_datum_vector(result);
	prepare_column(active, column, phase);
	datum = &active->datums[column];
	array = &active->columns[column];
	if (datum->values == NULL)
	{
		datum->values = MemoryContextAlloc(scan->batch_context,
										 sizeof(Datum) * bridge_batch->nrows);
		datum->isnull = MemoryContextAlloc(scan->batch_context,
										 sizeof(bool) * bridge_batch->nrows);
	}
	values = array->buffers[1];
	missing = rows[0] & ~datum->valid_rows;
	while (missing != 0)
	{
		int			row = pg_rightmost_one_pos64(missing);
		uint64		bit = UINT64CONST(1) << row;
		bool		isnull = !pg_batch_arrow_row_is_valid(array, row);

		datum->values[row] = isnull ? (Datum) 0 :
			Int32GetDatum(values[array->offset + row]);
		datum->isnull[row] = isnull;
		datum->valid_rows |= bit;
		if (phase == PG_BATCH_COLUMN_FILTER)
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

static void
release_batch(PgBatch *bridge_batch)
{
	ActiveBatch *active = bridge_batch->private_data;

	if (active != NULL)
		active->scan->active = NULL;
}

static const PgBatchOps batch_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_OPS_ABI_VERSION, PgBatchOps),
	.prepare_columns = prepare_columns,
	.get_datum_column = get_datum_column,
	.get_native_interface = get_native_interface,
	.release = release_batch,
};

static void
scan_cleanup(void *arg)
{
	PgBatchFdwScan *scan = arg;
	IpcReader  *reader = &scan->reader;

	if (scan->cleaned)
		return;
	scan->cleaned = true;
	release_record(scan);
	if (reader->schema_initialized)
		ArrowSchemaRelease(&reader->schema);
	if (reader->decoder_initialized)
		ArrowIpcDecoderReset(&reader->decoder);
	ArrowBufferReset(&reader->header);
	ArrowBufferReset(&reader->body);
	if (reader->file != NULL)
		FreeFile(reader->file);
	reader->file = NULL;
}

PgBatchFdwScan *
pg_batch_fdw_scan_begin(Relation relation, PlanState *parent,
						Node *source_private, List *source_exprs,
						const PgBatchRequest *request,
						const AttrNumber *source_attnums,
						int nsource_columns,
						MemoryContext query_context)
{
	MemoryContext context;
	MemoryContext oldcontext;
	PgBatchFdwScan *scan;
	List	   *specs = castNode(List, source_private);
	char	   *filename;
	int			qualno = 0;
	int			natts = RelationGetNumberOfAttributes(relation);

	pg_batch_fdw_validate_relation(relation);
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only superusers may read local files with pg_batch_fdw")));
	context = AllocSetContextCreate(query_context, "pg_batch FDW scan",
									ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(context);
	scan = palloc0_object(PgBatchFdwScan);
	scan->context = context;
	scan->batch_context = AllocSetContextCreate(context,
											"pg_batch FDW batch",
											ALLOCSET_DEFAULT_SIZES);
	scan->relation = relation;
	scan->econtext = parent->ps_ExprContext;
	scan->request = request;
	scan->source_attnums = source_attnums;
	scan->ncolumns = nsource_columns;
	scan->record_columns = palloc0_array(ArrowArray, natts);
	scan->record_decoded = palloc0_array(bool, natts);
	scan->nquals = list_length(specs);
	scan->quals = palloc0_array(SourceQual, scan->nquals);
	foreach_int(exprno, specs)
	{
		SourceQual *qual = &scan->quals[qualno++];

		qual->expr = pg_batch_expr_compile_filter(
			list_nth(source_exprs, exprno), parent, resolve_source_var,
			scan);
		qual->column = pg_batch_expr_input_column(qual->expr);
		qual->column_mask = bms_make_singleton(qual->column);
	}
	scan->cleanup.func = scan_cleanup;
	scan->cleanup.arg = scan;
	MemoryContextRegisterResetCallback(context, &scan->cleanup);
	filename = pg_batch_fdw_get_filename(RelationGetRelid(relation));
	reader_init(scan, filename);
	pfree(filename);
	MemoryContextSwitchTo(oldcontext);
	return scan;
}

PgBatch *
pg_batch_fdw_scan_next(PgBatchFdwScan *scan)
{
	ActiveBatch *active;
	int			nrows;
	int			ncolumns = scan->ncolumns;

	if (scan->active != NULL)
		elog(ERROR, "pg_batch_fdw batch was not released by its consumer");
	for (;;)
	{
		while (scan->record_offset >= scan->record_length)
		{
			if (!load_record(scan))
				return NULL;
			if (scan->record_length == 0)
				continue;
		}
		MemoryContextReset(scan->batch_context);
		active = MemoryContextAllocZero(scan->batch_context, sizeof(*active));
		active->scan = scan;
		active->start = scan->record_offset;
		nrows = Min(scan->request->max_rows > 0 ?
					Min(scan->request->max_rows, PG_BATCH_FDW_BATCH_SIZE) :
					PG_BATCH_FDW_BATCH_SIZE,
					scan->record_length - scan->record_offset);
		if (ncolumns > 0)
		{
			active->columns = MemoryContextAllocZero(scan->batch_context,
												  sizeof(ArrowArray) * ncolumns);
			active->prepared = MemoryContextAllocZero(scan->batch_context,
												   sizeof(bool) * ncolumns);
			active->datums = MemoryContextAllocZero(scan->batch_context,
												 sizeof(DatumColumn) * ncolumns);
		}
		active->selection = UINT64_MAX >> (64 - nrows);
		active->bridge_batch.abi_version = PG_BATCH_ABI_VERSION;
		active->bridge_batch.struct_size = sizeof(PgBatch);
		active->bridge_batch.nrows = nrows;
		active->bridge_batch.nwords = 1;
		active->bridge_batch.selection = &active->selection;
		active->bridge_batch.table_oid = RelationGetRelid(scan->relation);
		active->bridge_batch.ops = &batch_ops;
		active->bridge_batch.private_data = active;
		scan->active = active;
		apply_source_quals(active);
		scan->record_offset += nrows;
		scan->stats.batch_windows++;
		if (active->selection != 0)
			return &active->bridge_batch;
		/* A TupleTableSlot cannot represent a batch with no visible row. */
		scan->active = NULL;
	}
}

void
pg_batch_fdw_scan_rescan(PgBatchFdwScan *scan)
{
	if (scan->active != NULL)
		elog(ERROR, "pg_batch_fdw cannot rescan with an active batch");
	reader_reset(scan);
	scan->scalar_row = -1;
}

void
pg_batch_fdw_scan_end(PgBatchFdwScan *scan)
{
	MemoryContext context = scan->context;

	MemoryContextUnregisterResetCallback(context, &scan->cleanup);
	scan_cleanup(scan);
	MemoryContextDelete(context);
}

void
pg_batch_fdw_scan_explain(PgBatchFdwScan *scan, ExplainState *es)
{
	int			natts = RelationGetNumberOfAttributes(scan->relation);
	uint64		possible = scan->stats.record_batches * natts;

	ExplainPropertyText("Native Format", "Arrow IPC int32", es);
	ExplainPropertyText("Source Filter", scan->nquals > 0 ? "exact" : "off", es);
	ExplainPropertyText("Column Decoding",
						pg_batch_fdw_column_pruning ? "lazy" : "eager", es);
	if (!es->analyze)
		return;
	ExplainPropertyInteger("Arrow Record Batches", NULL,
						   scan->stats.record_batches, es);
	ExplainPropertyInteger("Arrow Batch Windows", NULL,
						   scan->stats.batch_windows, es);
	ExplainPropertyInteger("Rows Removed by Source Filter", NULL,
						   scan->stats.rows_removed, es);
	ExplainPropertyInteger("Arrow Bytes Read", NULL, scan->stats.bytes_read, es);
	ExplainPropertyInteger("Arrow Columns Decoded", NULL,
						   scan->stats.columns_decoded, es);
	ExplainPropertyInteger("Arrow Filter Columns", NULL,
						   scan->stats.filter_columns_decoded, es);
	ExplainPropertyInteger("Arrow Projection Columns", NULL,
						   scan->stats.project_columns_decoded, es);
	ExplainPropertyInteger("Arrow Columns Skipped", NULL,
						   possible >= scan->stats.columns_decoded ?
						   possible - scan->stats.columns_decoded : 0, es);
	ExplainPropertyInteger("Filter Datums", NULL, scan->stats.filter_datums, es);
	ExplainPropertyInteger("Projection Datums", NULL,
						   scan->stats.project_datums, es);
}

void
pg_batch_fdw_begin_foreign_scan(ForeignScanState *node, int eflags)
{
	ForeignScan *plan = castNode(ForeignScan, node->ss.ps.plan);
	MemoryContext oldcontext;
	PgBatchRequest *request;
	PgBatchLayout *layout;
	AttrNumber *attnums;
	int			natts;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	natts = RelationGetNumberOfAttributes(node->ss.ss_currentRelation);
	oldcontext = MemoryContextSwitchTo(node->ss.ps.state->es_query_cxt);
	request = palloc0_object(PgBatchRequest);
	request->struct_size = sizeof(*request);
	layout = palloc0_object(PgBatchLayout);
	layout->struct_size = sizeof(*layout);
	layout->ncolumns = natts;
	layout->ntargets = natts;
	request->layout = layout;
	attnums = palloc_array(AttrNumber, natts);
	for (int i = 0; i < natts; i++)
		attnums[i] = i + 1;
	node->fdw_state = pg_batch_fdw_scan_begin(node->ss.ss_currentRelation,
										  &node->ss.ps,
										  (Node *) plan->fdw_private,
										  plan->fdw_exprs, request,
										  attnums, natts,
										  node->ss.ps.state->es_query_cxt);
	MemoryContextSwitchTo(oldcontext);
}

TupleTableSlot *
pg_batch_fdw_iterate_foreign_scan(ForeignScanState *node)
{
	PgBatchFdwScan *scan = node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	int			natts = RelationGetNumberOfAttributes(node->ss.ss_currentRelation);

	for (;;)
	{
		PgBatch *batch;
		int			row;

		if (scan->active == NULL)
		{
			batch = pg_batch_fdw_scan_next(scan);
			if (batch == NULL)
				return ExecClearTuple(slot);
			scan->scalar_row = -1;
		}
		batch = &scan->active->bridge_batch;
		row = pg_batch_next_selected(batch, scan->scalar_row);
		if (row < 0)
		{
			release_batch(batch);
			continue;
		}
		scan->scalar_row = row;
		ExecClearTuple(slot);
		for (int column = 0; column < natts; column++)
		{
			PgBatchDatumVector datum;
			uint64		rows = UINT64CONST(1) << row;

			pg_batch_get_datum_column(batch, column, &rows,
				PG_BATCH_COLUMN_PROJECT, &datum);
			slot->tts_values[column] = datum.values[row];
			slot->tts_isnull[column] = datum.isnull[row];
		}
		ExecStoreVirtualTuple(slot);
		slot->tts_tableOid = RelationGetRelid(node->ss.ss_currentRelation);
		return slot;
	}
}

void
pg_batch_fdw_rescan_foreign_scan(ForeignScanState *node)
{
	PgBatchFdwScan *scan = node->fdw_state;

	if (scan->active != NULL)
		release_batch(&scan->active->bridge_batch);
	pg_batch_fdw_scan_rescan(scan);
}

void
pg_batch_fdw_end_foreign_scan(ForeignScanState *node)
{
	PgBatchFdwScan *scan = node->fdw_state;

	if (scan == NULL)
		return;
	if (scan->active != NULL)
		release_batch(&scan->active->bridge_batch);
	pg_batch_fdw_scan_end(scan);
	node->fdw_state = NULL;
}

void
pg_batch_fdw_explain_foreign_scan(ForeignScanState *node, ExplainState *es)
{
	PgBatchFdwScan *scan = node->fdw_state;

	if (scan != NULL)
		pg_batch_fdw_scan_explain(scan, es);
	else
	{
		ForeignScan *plan = castNode(ForeignScan, node->ss.ps.plan);

		ExplainPropertyText("Native Format", "Arrow IPC int32", es);
		ExplainPropertyText("Source Filter",
							plan->fdw_private != NIL ? "exact" : "off", es);
		ExplainPropertyText("Column Decoding",
							pg_batch_fdw_column_pruning ? "lazy" : "eager", es);
	}
}
