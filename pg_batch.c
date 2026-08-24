#include "postgres.h"

#include "fmgr.h"
#include "utils/guc.h"

#include "pg_batch.h"

PG_MODULE_MAGIC;

PGDLLEXPORT void _PG_init(void);
PGDLLEXPORT void _PG_fini(void);

bool		pg_batch_enable = true;

void
_PG_init(void)
{
	DefineCustomBoolVariable("pg_batch.enable",
							 "Enable pg_batch custom paths.", NULL,
							 &pg_batch_enable, true, PGC_USERSET, 0,
							 NULL, NULL, NULL);
	pg_batch_planner_init();
}

void
_PG_fini(void)
{
	pg_batch_planner_fini();
}
