#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeCustom.h"

#include "internal.h"

void
pg_batch_get_int4_vector(PgBatchBridgeBatch *batch, int column,
						 const uint64 *selected_rows,
						 PgBatchMaterializePhase phase,
						 PgBatchInt4Vector *result)
{
	PgBatchArrowView arrow;

	if (unlikely(batch->ops->get_native_interface != NULL) &&
		pg_batch_get_arrow_column(batch, column, &arrow))
	{
		const struct ArrowArray *array = arrow.array;

		if (strcmp(arrow.schema->format, "i") != 0)
			elog(ERROR, "pg_batch int4 kernel received a non-int4 Arrow column");
		result->layout = PG_BATCH_INT4_PACKED;
		result->data.packed.values = array->buffers[1];
		result->data.packed.validity =
			array->null_count == 0 ? NULL : array->buffers[0];
		result->data.packed.offset = array->offset;
	}
	else
	{
		PgBatchBridgeDatumColumn datum;

		pg_batch_get_datum_column(batch, column, selected_rows, phase, &datum);
		result->layout = PG_BATCH_INT4_DATUM;
		result->data.datum.values = datum.values;
		result->data.datum.isnull = datum.isnull;
		result->data.datum.available = datum.valid_rows;
		result->data.datum.nwords = datum.nwords;
	}
}

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
