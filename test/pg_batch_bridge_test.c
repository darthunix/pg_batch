#include "postgres.h"

#include "fmgr.h"

#include "bridge.h"
#include "source.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_batch_bridge_test_bad_abi);
PG_FUNCTION_INFO_V1(pg_batch_bridge_test_compatible_sizes);
PG_FUNCTION_INFO_V1(pg_batch_bridge_test_duplicate_provider);

static const PgBatchAPI *
get_bridge(void)
{
	void	  **rendezvous;
	const PgBatchAPI *api;

	load_file("$libdir/pg_batch_api", false);
	rendezvous = find_rendezvous_variable(PG_BATCH_RENDEZVOUS);
	api = *rendezvous;
	if (api == NULL || api->abi_version != PG_BATCH_API_ABI_VERSION ||
		api->struct_size < PG_BATCH_API_MIN_SIZE)
		elog(ERROR, "pg_batch bridge is not available to its test module");
	return api;
}

static bool
supports_nothing(Relation relation)
{
	return false;
}

static void
plan_nothing(const PgBatchSourcePlanRequest *request,
			 PgBatchSourcePlanResult *result)
{
}

static void *
begin_nothing(const PgBatchSourceExecRequest *request)
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

static const PgBatchProviderOps provider_one = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_PROVIDER_OPS_ABI_VERSION,
		PgBatchProviderOps),
	.provider_name = "pg_batch_bridge_test_duplicate",
	.supports_relation = supports_nothing,
	.plan_scan = plan_nothing,
	.begin_scan = begin_nothing,
	.rescan = rescan_nothing,
	.end_scan = end_nothing,
};

static const PgBatchProviderOps provider_two = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_PROVIDER_OPS_ABI_VERSION,
		PgBatchProviderOps),
	.provider_name = "pg_batch_bridge_test_duplicate",
	.supports_relation = supports_nothing,
	.plan_scan = plan_nothing,
	.begin_scan = begin_nothing,
	.rescan = rescan_nothing,
	.end_scan = end_nothing,
};

typedef struct ExtendedProviderOps
{
	PgBatchProviderOps base;
	void	   *future_callback;
} ExtendedProviderOps;

Datum
pg_batch_bridge_test_bad_abi(PG_FUNCTION_ARGS)
{
	PgBatchProviderOps invalid = provider_one;

	invalid.abi_version++;
	invalid.provider_name = "pg_batch_bridge_test_bad_abi";
	get_bridge()->register_provider(&invalid);
	PG_RETURN_VOID();
}

Datum
pg_batch_bridge_test_compatible_sizes(PG_FUNCTION_ARGS)
{
	const PgBatchAPI *api = get_bridge();
	PgBatchProviderOps truncated = provider_one;
	ExtendedProviderOps extended = {
		.base = provider_two,
		.future_callback = NULL,
	};

	truncated.provider_name = "pg_batch_bridge_test_truncated";
	truncated.struct_size = PG_BATCH_PROVIDER_OPS_MIN_SIZE;
	extended.base.provider_name = "pg_batch_bridge_test_extended";
	extended.base.struct_size = sizeof(extended);
	api->register_provider(&truncated);
	api->unregister_provider(truncated.provider_name);
	api->register_provider(&extended.base);
	api->unregister_provider(extended.base.provider_name);
	PG_RETURN_BOOL(true);
}

Datum
pg_batch_bridge_test_duplicate_provider(PG_FUNCTION_ARGS)
{
	const PgBatchAPI *api = get_bridge();

	api->register_provider(&provider_one);
	api->register_provider(&provider_two);
	PG_RETURN_VOID();
}
