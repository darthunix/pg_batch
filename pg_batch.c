#include "postgres.h"

#include "fmgr.h"
#include "utils/guc.h"

#include "pg_batch.h"

PG_MODULE_MAGIC;

PGDLLEXPORT void _PG_init(void);
PGDLLEXPORT void _PG_fini(void);

bool		pg_batch_enable = true;
bool		pg_batch_use_compressed = false;
bool		pg_batch_compressed_via_tableam = false;
int			pg_batch_compressed_scan_mode = PG_BATCH_COMPRESSED_BATCH;

static const struct config_enum_entry pg_batch_compressed_scan_modes[] = {
	{"batch", PG_BATCH_COMPRESSED_BATCH, false},
	{"prune", PG_BATCH_COMPRESSED_PRUNE, false},
	{"filter", PG_BATCH_COMPRESSED_FILTER, false},
	{NULL, 0, false}
};

void
_PG_init(void)
{
	DefineCustomBoolVariable("pg_batch.enable",
							 "Enable pg_batch custom paths.", NULL,
							 &pg_batch_enable, true, PGC_USERSET, 0,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("pg_batch.use_compressed",
							 "Use a backend-local compressed Arrow snapshot when available.",
							 NULL, &pg_batch_use_compressed, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomEnumVariable("pg_batch.compressed_scan_mode",
							 "Control group pruning and source filtering for compressed snapshots.",
							 NULL, &pg_batch_compressed_scan_mode,
							 PG_BATCH_COMPRESSED_BATCH,
							 pg_batch_compressed_scan_modes,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("pg_batch.compressed_via_tableam",
							 "Read compressed snapshots through the test table access method.",
							 NULL, &pg_batch_compressed_via_tableam, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	pg_batch_planner_init();
}

void
_PG_fini(void)
{
	pg_batch_planner_fini();
	pg_batch_compress_fini();
}
