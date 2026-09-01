#include "postgres.h"

#include "fmgr.h"
#include "utils/guc.h"

#include "internal.h"

PG_MODULE_MAGIC;

PGDLLEXPORT void _PG_init(void);
PGDLLEXPORT void _PG_fini(void);

bool		pg_batch_enable = true;
bool		pg_batch_enable_hash_join = true;
bool		pg_batch_enable_simd = true;
double		pg_batch_bitmap_min_rows_per_page = 8.0;
const PgBatchAPI *pg_batch_api;

void
_PG_init(void)
{
	pg_batch_api = pg_batch_api_get();
	DefineCustomBoolVariable("pg_batch.enable",
							 "Enable pg_batch custom paths.", NULL,
							 &pg_batch_enable, true, PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("pg_batch.enable_hash_join",
							 "Enable pg_batch hash join paths.", NULL,
							 &pg_batch_enable_hash_join, true, PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("pg_batch.enable_simd",
							 "Use SIMD for dense supported batch filters.", NULL,
							 &pg_batch_enable_simd, true, PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomRealVariable("pg_batch.bitmap_min_rows_per_page",
							 "Minimum estimated rows per heap page for a "
							 "batch bitmap path.",
							 NULL, &pg_batch_bitmap_min_rows_per_page,
							 8.0, 0.0, PG_BATCH_SIZE, PGC_USERSET, 0,
								 NULL, NULL, NULL);
	pg_batch_planner_init();
	pg_batch_api->register_node(&pg_batch_node_ops);
}

void
_PG_fini(void)
{
	if (pg_batch_api != NULL)
		pg_batch_api->unregister_node(PG_BATCH_NODE_NAME);
	pg_batch_planner_fini();
}
