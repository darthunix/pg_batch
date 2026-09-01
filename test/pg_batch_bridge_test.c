#include "postgres.h"

#include "fmgr.h"

#include "bridge.h"
#include "source.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_batch_bridge_test_bad_abi);
PG_FUNCTION_INFO_V1(pg_batch_bridge_test_compatible_sizes);
PG_FUNCTION_INFO_V1(pg_batch_bridge_test_duplicate_source);

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
rescan_nothing(void *source_state)
{
}

static void
end_nothing(void *source_state)
{
}

static const PgBatchSourceOps source_one = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_SOURCE_OPS_ABI_VERSION,
		PgBatchSourceOps),
	.source_name = "pg_batch_bridge_test_duplicate",
	.supports_relation = supports_nothing,
	.plan_scan = plan_nothing,
	.begin_scan = begin_nothing,
	.rescan = rescan_nothing,
	.end_scan = end_nothing,
};

static const PgBatchSourceOps source_two = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_SOURCE_OPS_ABI_VERSION,
		PgBatchSourceOps),
	.source_name = "pg_batch_bridge_test_duplicate",
	.supports_relation = supports_nothing,
	.plan_scan = plan_nothing,
	.begin_scan = begin_nothing,
	.rescan = rescan_nothing,
	.end_scan = end_nothing,
};

typedef struct ExtendedSourceOps
{
	PgBatchSourceOps base;
	void	   *future_callback;
} ExtendedSourceOps;

Datum
pg_batch_bridge_test_bad_abi(PG_FUNCTION_ARGS)
{
	PgBatchSourceOps invalid = source_one;

	invalid.abi_version++;
	invalid.source_name = "pg_batch_bridge_test_bad_abi";
	get_bridge()->register_source(&invalid);
	PG_RETURN_VOID();
}

Datum
pg_batch_bridge_test_compatible_sizes(PG_FUNCTION_ARGS)
{
	const PgBatchAPI *api = get_bridge();
	PgBatchSourceOps truncated = source_one;
	ExtendedSourceOps extended = {
		.base = source_two,
		.future_callback = NULL,
	};

	truncated.source_name = "pg_batch_bridge_test_truncated";
	truncated.struct_size = PG_BATCH_SOURCE_OPS_MIN_SIZE;
	extended.base.source_name = "pg_batch_bridge_test_extended";
	extended.base.struct_size = sizeof(extended);
	api->register_source(&truncated);
	api->unregister_source(truncated.source_name);
	api->register_source(&extended.base);
	api->unregister_source(extended.base.source_name);
	PG_RETURN_BOOL(true);
}

Datum
pg_batch_bridge_test_duplicate_source(PG_FUNCTION_ARGS)
{
	const PgBatchAPI *api = get_bridge();

	api->register_source(&source_one);
	api->register_source(&source_two);
	PG_RETURN_VOID();
}
