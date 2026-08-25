#include "postgres.h"

#include "access/heapam.h"
#include "access/tableam.h"
#include "fmgr.h"
#include "utils/rel.h"

#include "pg_batch.h"

PG_FUNCTION_INFO_V1(pg_batch_tableam_handler);

static TableAmRoutine pg_batch_tableam_methods;
static bool pg_batch_tableam_initialized;

/*
 * This is deliberately a heap-compatible test AM, not a second storage
 * implementation. Ordinary callers use every heap callback unchanged. A
 * PgBatchSlot passed to scan_getnextslot carries the source request and lets
 * the same compressed reader used by the direct path return a whole batch.
 */

static bool
pg_batch_tableam_getnextslot(TableScanDesc scan, ScanDirection direction,
							 TupleTableSlot *slot)
{
	if (slot->tts_ops != &PgBatchSlotOps)
		return heap_getnextslot(scan, direction, slot);
	if (direction != ForwardScanDirection)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("pg_batch compressed scans support only forward scans")));
	return pg_batch_compressed_scan_next((PgBatchSlot *) slot, scan->rs_rd);
}

static void
pg_batch_init_tableam(void)
{
	if (pg_batch_tableam_initialized)
		return;
	pg_batch_tableam_methods = *GetHeapamTableAmRoutine();
	pg_batch_tableam_methods.scan_getnextslot = pg_batch_tableam_getnextslot;
	pg_batch_tableam_initialized = true;
}

Datum
pg_batch_tableam_handler(PG_FUNCTION_ARGS)
{
	pg_batch_init_tableam();
	PG_RETURN_POINTER(&pg_batch_tableam_methods);
}

bool
pg_batch_relation_uses_tableam(Relation relation)
{
	pg_batch_init_tableam();
	return relation->rd_tableam == &pg_batch_tableam_methods;
}
