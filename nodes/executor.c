#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeCustom.h"

#include "internal.h"

TupleTableSlot *
pg_batch_result_batch_slot(PlanState *planstate)
{
	TupleTableSlot *slot = planstate->ps_ResultTupleSlot;

	if (slot != NULL && slot->tts_ops == &PgBatchSlotOps)
		return slot;
	if (IsA(planstate, CustomScanState))
	{
		CustomScanState *custom = castNode(CustomScanState, planstate);
		TupleTableSlot *batch_slot = pg_batch_hash_join_output_slot(custom);

		if (batch_slot != NULL)
			return batch_slot;
		slot = custom->ss.ss_ScanTupleSlot;
		if (slot != NULL && slot->tts_ops == &PgBatchSlotOps)
			return slot;
	}
	elog(ERROR, "pg_batch child does not publish batches");
	return NULL;
}

void
pg_batch_init_children(CustomScanState *css, EState *estate, int eflags)
{
	CustomScan *cscan = castNode(CustomScan, css->ss.ps.plan);
	ListCell   *lc;

	foreach(lc, cscan->custom_plans)
		css->custom_ps = lappend(css->custom_ps,
								 ExecInitNode((Plan *) lfirst(lc), estate, eflags));
}

void
pg_batch_end_children(CustomScanState *css)
{
	ListCell   *lc;

	foreach(lc, css->custom_ps)
		ExecEndNode(lfirst(lc));
}

void
pg_batch_rescan_children(CustomScanState *css)
{
	ListCell   *lc;

	foreach(lc, css->custom_ps)
		ExecReScan(lfirst(lc));
}
