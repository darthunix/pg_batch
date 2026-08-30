#include "postgres.h"

#include "access/reloptions.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "foreign/foreign.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "internal.h"

PG_MODULE_MAGIC;

PGDLLEXPORT void _PG_init(void);
PGDLLEXPORT void _PG_fini(void);

PG_FUNCTION_INFO_V1(pg_batch_fdw_handler);
PG_FUNCTION_INFO_V1(pg_batch_fdw_validator);

bool		pg_batch_fdw_pushdown = true;
bool		pg_batch_fdw_column_pruning = true;
const PgBatchAPI *pg_batch_fdw_api;

void
_PG_init(void)
{
	pg_batch_fdw_api = pg_batch_api_get();
	DefineCustomBoolVariable("pg_batch_fdw.pushdown",
							 "Apply supported restrictions inside the Arrow source.",
							 NULL, &pg_batch_fdw_pushdown, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("pg_batch_fdw.column_pruning",
							 "Decode Arrow columns only when a consumer requests them.",
							 NULL, &pg_batch_fdw_column_pruning, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	pg_batch_fdw_api->register_provider(&pg_batch_fdw_provider_ops);
}

void
_PG_fini(void)
{
	if (pg_batch_fdw_api != NULL)
		pg_batch_fdw_api->unregister_provider(PG_BATCH_FDW_PROVIDER_NAME);
}

Datum
pg_batch_fdw_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(pg_batch_fdw_routine());
}

Datum
pg_batch_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	bool		filename_seen = false;

	foreach_ptr(DefElem, option, options)
	{
		if (catalog != ForeignTableRelationId ||
			strcmp(option->defname, "filename") != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", option->defname),
					 errhint("Only foreign tables accept the \"filename\" option.")));
		if (filename_seen)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("conflicting or redundant options")));
		filename_seen = true;
	}
	if (catalog == ForeignTableRelationId && !filename_seen)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_DYNAMIC_PARAMETER_VALUE_NEEDED),
				 errmsg("filename is required for a pg_batch_fdw foreign table")));
	PG_RETURN_VOID();
}

char *
pg_batch_fdw_get_filename(Oid relation_oid)
{
	ForeignTable *table = GetForeignTable(relation_oid);

	foreach_ptr(DefElem, option, table->options)
	{
		if (strcmp(option->defname, "filename") == 0)
			return pstrdup(defGetString(option));
	}
	elog(ERROR, "pg_batch_fdw foreign table has no filename");
	return NULL;
}

void
pg_batch_fdw_validate_relation(Relation relation)
{
	TupleDesc	desc = RelationGetDescr(relation);

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, i);

		if (attr->attisdropped || attr->atttypid != INT4OID)
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("pg_batch_fdw supports only non-dropped int4 columns"),
					 errdetail("Column %d of relation \"%s\" is not supported.",
							   i + 1, RelationGetRelationName(relation))));
	}
}
