#include "postgres.h"

#include "fmgr.h"
#include "utils/guc.h"

#include "internal.h"

PG_MODULE_MAGIC;

PGDLLEXPORT void _PG_init(void);
PGDLLEXPORT void _PG_fini(void);

bool		pg_batch_tam_enable = true;
int			pg_batch_tam_scan_mode = PG_BATCH_TAM_BATCH;
const PgBatchAPI *pg_batch_tam_api;

static const struct config_enum_entry pg_batch_tam_scan_modes[] = {
	{"batch", PG_BATCH_TAM_BATCH, false},
	{"prune", PG_BATCH_TAM_PRUNE, false},
	{"filter", PG_BATCH_TAM_FILTER, false},
	{NULL, 0, false}
};

void
_PG_init(void)
{
	pg_batch_tam_api = pg_batch_api_get();
	DefineCustomBoolVariable("pg_batch_tam.enable",
							 "Let the compressed test TAM provide native batches.",
							 NULL, &pg_batch_tam_enable, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomEnumVariable("pg_batch_tam.scan_mode",
							 "Control storage-group pruning and exact source filtering.",
							 NULL, &pg_batch_tam_scan_mode,
							 PG_BATCH_TAM_BATCH, pg_batch_tam_scan_modes,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	pg_batch_tam_api->register_provider(&pg_batch_tam_provider_ops);
}

void
_PG_fini(void)
{
	if (pg_batch_tam_api != NULL)
		pg_batch_tam_api->unregister_provider(PG_BATCH_TAM_PROVIDER_NAME);
	pg_batch_compressed_fini();
}
