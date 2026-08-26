#include "postgres.h"

#include "access/heapam.h"
#include "access/tableam.h"
#include "fmgr.h"
#include "utils/rel.h"

#include "internal.h"

PG_FUNCTION_INFO_V1(pg_batch_tableam_handler);

static TableAmRoutine pg_batch_tableam_methods;
static bool pg_batch_tableam_initialized;

/*
 * This is deliberately a heap-compatible test AM, not a second storage
 * implementation. Ordinary callers use every heap callback unchanged. A
 * batch-aware scan attaches its request to an otherwise ordinary slot
 * through pg_batch_bridge. The TAM publishes a batch through the same bridge;
 * neither side needs the other extension's private slot definition.
 */

static bool
compressed_getnextslot(TableScanDesc scan, ScanDirection direction,
					   TupleTableSlot *slot)
{
	PgBatchBridgeBinding *binding = pg_batch_tam_bridge->find_binding(slot);
	const PgBatchBridgeRequest *request =
		pg_batch_tam_bridge->get_request(binding);

	if (request == NULL || request->provider_name == NULL ||
		strcmp(request->provider_name, PG_BATCH_TAM_PROVIDER_NAME) != 0)
		return heap_getnextslot(scan, direction, slot);
	if (direction != ForwardScanDirection)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_batch compressed scans support only forward scans")));
	if (request->provider_state == NULL)
		elog(ERROR, "pg_batch TAM request has no provider state");
	return pg_batch_compressed_scan_next(binding, request->provider_state);
}

static void
init_tableam(void)
{
	if (pg_batch_tableam_initialized)
		return;
	pg_batch_tableam_methods = *GetHeapamTableAmRoutine();
	pg_batch_tableam_methods.scan_getnextslot = compressed_getnextslot;
	pg_batch_tableam_initialized = true;
}

Datum
pg_batch_tableam_handler(PG_FUNCTION_ARGS)
{
	init_tableam();
	PG_RETURN_POINTER(&pg_batch_tableam_methods);
}

bool
pg_batch_relation_uses_tableam(Relation relation)
{
	init_tableam();
	return relation->rd_tableam == &pg_batch_tableam_methods;
}
