#include "postgres.h"

#include "fmgr.h"
#include "utils/guc.h"

#include "internal.h"

PG_MODULE_MAGIC;

PGDLLEXPORT void _PG_init(void);
PGDLLEXPORT void _PG_fini(void);

bool		pg_batch_limit_enable = true;
const PgBatchAPI *pg_batch_limit_api;

void
_PG_init(void)
{
	pg_batch_limit_api = pg_batch_api_get();
	DefineCustomBoolVariable("pg_batch_limit.enable",
							 "Use the independent batch-aware Limit node.",
							 NULL, &pg_batch_limit_enable, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	pg_batch_limit_planner_init();
	pg_batch_limit_api->register_node(&pg_batch_limit_node_ops);
}

void
_PG_fini(void)
{
	if (pg_batch_limit_api != NULL)
		pg_batch_limit_api->unregister_node(
			PG_BATCH_LIMIT_NODE_NAME);
	pg_batch_limit_planner_fini();
}
