#include "postgres.h"

#include "fmgr.h"

#include "bridge.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_batch_bridge_test_bad_abi);
PG_FUNCTION_INFO_V1(pg_batch_bridge_test_duplicate_provider);

static const PgBatchBridgeAPI *
get_bridge(void)
{
	void	  **rendezvous;
	const PgBatchBridgeAPI *api;

	load_file("$libdir/pg_batch_bridge", false);
	rendezvous = find_rendezvous_variable(PG_BATCH_BRIDGE_RENDEZVOUS);
	api = *rendezvous;
	if (api == NULL || api->abi_version != PG_BATCH_BRIDGE_ABI_VERSION ||
		api->struct_size < sizeof(PgBatchBridgeAPI))
		elog(ERROR, "pg_batch bridge is not available to its test module");
	return api;
}

static bool
supports_nothing(Relation relation)
{
	return false;
}

static void
plan_nothing(const PgBatchBridgePlanRequest *request,
			 PgBatchBridgePlanResult *result)
{
}

static void *
begin_nothing(const PgBatchBridgeExecRequest *request)
{
	return NULL;
}

static void
rescan_nothing(void *provider_state)
{
}

static void
end_nothing(void *provider_state)
{
}

static const PgBatchBridgeProviderOps provider_one = {
	.abi_version = PG_BATCH_BRIDGE_ABI_VERSION,
	.struct_size = sizeof(PgBatchBridgeProviderOps),
	.provider_name = "pg_batch_bridge_test_duplicate",
	.supports_relation = supports_nothing,
	.plan_scan = plan_nothing,
	.begin_scan = begin_nothing,
	.rescan = rescan_nothing,
	.end_scan = end_nothing,
};

static const PgBatchBridgeProviderOps provider_two = {
	.abi_version = PG_BATCH_BRIDGE_ABI_VERSION,
	.struct_size = sizeof(PgBatchBridgeProviderOps),
	.provider_name = "pg_batch_bridge_test_duplicate",
	.supports_relation = supports_nothing,
	.plan_scan = plan_nothing,
	.begin_scan = begin_nothing,
	.rescan = rescan_nothing,
	.end_scan = end_nothing,
};

Datum
pg_batch_bridge_test_bad_abi(PG_FUNCTION_ARGS)
{
	PgBatchBridgeProviderOps invalid = provider_one;

	invalid.abi_version++;
	invalid.provider_name = "pg_batch_bridge_test_bad_abi";
	get_bridge()->register_provider(&invalid);
	PG_RETURN_VOID();
}

Datum
pg_batch_bridge_test_duplicate_provider(PG_FUNCTION_ARGS)
{
	const PgBatchBridgeAPI *api = get_bridge();

	api->register_provider(&provider_one);
	api->register_provider(&provider_two);
	PG_RETURN_VOID();
}
