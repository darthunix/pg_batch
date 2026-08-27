#include "postgres.h"

#include "fmgr.h"
#include "utils/guc.h"

#include "internal.h"

PG_MODULE_MAGIC;

PGDLLEXPORT void _PG_init(void);
PGDLLEXPORT void _PG_fini(void);

bool		pg_batch_enable = true;
bool		pg_batch_enable_hash_join = true;
const PgBatchBridgeAPI *pg_batch_bridge;

static void
load_bridge(void)
{
	void	  **rendezvous = find_rendezvous_variable(PG_BATCH_BRIDGE_RENDEZVOUS);

	if (*rendezvous == NULL)
		load_file("$libdir/pg_batch_bridge", false);
	pg_batch_bridge = *rendezvous;
	if (pg_batch_bridge == NULL ||
		pg_batch_bridge->abi_version != PG_BATCH_BRIDGE_ABI_VERSION ||
		pg_batch_bridge->struct_size < sizeof(PgBatchBridgeAPI))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("incompatible pg_batch_bridge library")));
}

void
_PG_init(void)
{
	load_bridge();
	DefineCustomBoolVariable("pg_batch.enable",
							 "Enable pg_batch custom paths.", NULL,
							 &pg_batch_enable, true, PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("pg_batch.enable_hash_join",
							 "Enable pg_batch hash join paths.", NULL,
							 &pg_batch_enable_hash_join, true, PGC_USERSET, 0,
								 NULL, NULL, NULL);
	pg_batch_planner_init();
}

void
_PG_fini(void)
{
	pg_batch_planner_fini();
}
