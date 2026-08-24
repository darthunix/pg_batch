#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeCustom.h"

#include "pg_batch.h"

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
