#include "postgres.h"

#include "fmgr.h"
#include "utils/guc.h"

#include "internal.h"

PG_MODULE_MAGIC;

PGDLLEXPORT void _PG_init(void);
PGDLLEXPORT void _PG_fini(void);

bool		pg_batch_tam_enable = true;
int			pg_batch_tam_scan_mode = PG_BATCH_TAM_BATCH;
const PgBatchBridgeAPI *pg_batch_tam_bridge;

static const struct config_enum_entry pg_batch_tam_scan_modes[] = {
	{"batch", PG_BATCH_TAM_BATCH, false},
	{"prune", PG_BATCH_TAM_PRUNE, false},
	{"filter", PG_BATCH_TAM_FILTER, false},
	{NULL, 0, false}
};

static void
load_bridge(void)
{
	void	  **rendezvous = find_rendezvous_variable(PG_BATCH_BRIDGE_RENDEZVOUS);

	if (*rendezvous == NULL)
		load_file("$libdir/pg_batch_bridge", false);
	pg_batch_tam_bridge = *rendezvous;
	if (pg_batch_tam_bridge == NULL ||
		pg_batch_tam_bridge->abi_version != PG_BATCH_BRIDGE_ABI_VERSION ||
		pg_batch_tam_bridge->struct_size < sizeof(PgBatchBridgeAPI))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("incompatible pg_batch_bridge library")));
}

void
_PG_init(void)
{
	load_bridge();
	DefineCustomBoolVariable("pg_batch_tam.enable",
							 "Let the compressed test TAM provide native batches.",
							 NULL, &pg_batch_tam_enable, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomEnumVariable("pg_batch_tam.scan_mode",
							 "Control storage-group pruning and exact source filtering.",
							 NULL, &pg_batch_tam_scan_mode,
							 PG_BATCH_TAM_BATCH, pg_batch_tam_scan_modes,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	pg_batch_tam_bridge->register_provider(&pg_batch_tam_provider_ops);
}

void
_PG_fini(void)
{
	if (pg_batch_tam_bridge != NULL)
		pg_batch_tam_bridge->unregister_provider(PG_BATCH_TAM_PROVIDER_NAME);
	pg_batch_compressed_fini();
}
