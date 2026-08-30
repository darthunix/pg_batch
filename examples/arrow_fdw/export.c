#include "postgres.h"

#include <errno.h>
#include <unistd.h>

#include "access/table.h"
#include "access/tableam.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "internal.h"

PG_FUNCTION_INFO_V1(pg_batch_fdw_export);
PG_FUNCTION_INFO_V1(pg_batch_fdw_remove);

/* This writer exists only to make reproducible local IPC inputs for tests. */
static void
check_arrow(int code, ArrowError *error, const char *operation)
{
	if (code == NANOARROW_OK)
		return;
	ereport(ERROR,
			(errcode(ERRCODE_FDW_ERROR),
			 errmsg("pg_batch_fdw could not %s", operation),
			 errdetail("nanoarrow error %d: %s", code,
					   error->message[0] == '\0' ? "no detail" : error->message)));
}

static void
init_schema(Relation relation, ArrowSchema *schema)
{
	TupleDesc	desc = RelationGetDescr(relation);
	ArrowError	error;

	ArrowErrorInit(&error);
	ArrowSchemaInit(schema);
	check_arrow(ArrowSchemaSetTypeStruct(schema, desc->natts), &error,
				"create an Arrow schema");
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		check_arrow(ArrowSchemaSetType(schema->children[i],
									   NANOARROW_TYPE_INT32),
					&error, "create an Arrow int32 field");
		check_arrow(ArrowSchemaSetName(schema->children[i],
									   NameStr(attr->attname)),
					&error, "name an Arrow field");
		schema->children[i]->flags |= ARROW_FLAG_NULLABLE;
	}
}

Datum
pg_batch_fdw_export(PG_FUNCTION_ARGS)
{
	Oid			relation_oid = PG_GETARG_OID(0);
	char	   *filename = text_to_cstring(PG_GETARG_TEXT_PP(1));
	int32		batch_rows = PG_GETARG_INT32(2);
	Relation	relation;
	TableScanDesc scan;
	TupleTableSlot *slot;
	FILE	   *file;
	ArrowIpcOutputStream output;
	ArrowIpcWriter writer;
	ArrowSchema schema;
	ArrowError	error;
	int64		total_rows = 0;
	bool		done = false;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only superusers may write local files with pg_batch_fdw")));
	if (batch_rows <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Arrow record batch size must be greater than zero")));
	relation = table_open(relation_oid, AccessShareLock);
	if (relation->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("pg_batch_fdw_export source must be a table")));
	pg_batch_fdw_validate_relation(relation);
	file = AllocateFile(filename, "wb");
	if (file == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create pg_batch_fdw file \"%s\": %m", filename)));
	ArrowErrorInit(&error);
	output.release = NULL;
	writer.private_data = NULL;
	schema.release = NULL;
	check_arrow(ArrowIpcOutputStreamInitFile(&output, file, false), &error,
				"initialize the Arrow output stream");
	check_arrow(ArrowIpcWriterInit(&writer, &output), &error,
				"initialize the Arrow writer");
	init_schema(relation, &schema);
	check_arrow(ArrowIpcWriterWriteSchema(&writer, &schema, &error), &error,
				"write the Arrow schema");

	scan = table_beginscan(relation, GetActiveSnapshot(), 0, NULL, 0);
	slot = table_slot_create(relation, NULL);
	while (!done)
	{
		ArrowArray array;
		ArrowArrayView view;
		int			rows = 0;

		array.release = NULL;
		ArrowArrayViewInitFromType(&view, NANOARROW_TYPE_UNINITIALIZED);
		check_arrow(ArrowArrayInitFromSchema(&array, &schema, &error), &error,
					"initialize an Arrow record batch");
		check_arrow(ArrowArrayStartAppending(&array), &error,
					"start an Arrow record batch");
		while (rows < batch_rows)
		{
			if (!table_scan_getnextslot(scan, ForwardScanDirection, slot))
			{
				done = true;
				break;
			}
			for (int column = 0; column < array.n_children; column++)
			{
				bool		isnull;
				Datum		value = slot_getattr(slot, column + 1, &isnull);

				if (isnull)
					check_arrow(ArrowArrayAppendNull(array.children[column], 1),
								&error, "append an Arrow null");
				else
					check_arrow(ArrowArrayAppendInt(array.children[column],
												 DatumGetInt32(value)),
								&error, "append an Arrow int32 value");
			}
			check_arrow(ArrowArrayFinishElement(&array), &error,
						"finish an Arrow row");
			ExecClearTuple(slot);
			rows++;
			total_rows++;
		}
		if (rows == 0)
		{
			ArrowArrayRelease(&array);
			ArrowArrayViewReset(&view);
			break;
		}
		check_arrow(ArrowArrayFinishBuildingDefault(&array, &error), &error,
					"finish an Arrow record batch");
		check_arrow(ArrowArrayViewInitFromSchema(&view, &schema, &error), &error,
					"initialize an Arrow record view");
		check_arrow(ArrowArrayViewSetArray(&view, &array, &error), &error,
					"bind an Arrow record view");
		check_arrow(ArrowIpcWriterWriteArrayView(&writer, &view, &error), &error,
					"write an Arrow record batch");
		ArrowArrayViewReset(&view);
		ArrowArrayRelease(&array);
	}
	check_arrow(ArrowIpcWriterWriteArrayView(&writer, NULL, &error), &error,
				"finish the Arrow IPC stream");
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	ArrowSchemaRelease(&schema);
	ArrowIpcWriterReset(&writer);
	if (FreeFile(file) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close pg_batch_fdw file \"%s\": %m", filename)));
	table_close(relation, AccessShareLock);
	PG_RETURN_INT64(total_rows);
}

Datum
pg_batch_fdw_remove(PG_FUNCTION_ARGS)
{
	char	   *filename = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only superusers may remove local files with pg_batch_fdw")));
	if (unlink(filename) != 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove pg_batch_fdw file \"%s\": %m", filename)));
	PG_RETURN_VOID();
}
