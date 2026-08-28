#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "utils/datum.h"

#include "internal.h"

/*
 * PgBatchPack is the row-to-batch boundary used below batch-aware nodes.
 * A scalar child may reuse its result slot on every ExecProcNode() call, so
 * all non-NULL by-reference values are copied before the next row is fetched.
 */
typedef struct BatchPackState
{
	CustomScanState css;
	PlanState  *child;
	const PgBatchBridgeRequest *request;
	PgBatchBridgeBatch batch;
	uint64		selection;
	Datum	   *values;
	bool	   *isnull;
	uint64	   *valid_rows;
	int			ncolumns;
	bool		published;
	uint64		packed_batches;
	uint64		packed_rows;
} BatchPackState;

static const CustomExecMethods pack_exec_methods;

static void pack_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *pack_exec(CustomScanState *node);
static void pack_end(CustomScanState *node);
static void pack_rescan(CustomScanState *node);
static void pack_explain(CustomScanState *node, List *ancestors,
						 ExplainState *es);

Node *
pg_batch_create_pack_state(CustomScan *cscan)
{
	BatchPackState *state = palloc0_object(BatchPackState);

	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &pack_exec_methods;
	state->css.slotOps = &PgBatchSlotOps;
	return (Node *) state;
}

static const CustomExecMethods pack_exec_methods = {
	.CustomName = "PgBatchPack",
	.BeginCustomScan = pack_begin,
	.ExecCustomScan = pack_exec,
	.EndCustomScan = pack_end,
	.ReScanCustomScan = pack_rescan,
	.ExplainCustomScan = pack_explain,
};

static void
pack_prepare_columns(PgBatchBridgeBatch *batch, const Bitmapset *columns,
					 const uint64 *rows,
					 PgBatchBridgeMaterializePhase phase)
{
	/* Scalar rows have already been materialized while filling the batch. */
}

static void
pack_get_datum_column(PgBatchBridgeBatch *batch, int column,
					  const uint64 *rows,
					  PgBatchBridgeMaterializePhase phase,
					  PgBatchBridgeDatumColumn *result)
{
	BatchPackState *state = batch->private_data;

	if (column < 0 || column >= state->ncolumns)
		elog(ERROR, "pg_batch packed column is out of range");
	if ((state->valid_rows[column] & rows[0]) != rows[0])
		elog(ERROR, "pg_batch packed column is not materialized");
	result->values = &state->values[column * PG_BATCH_SIZE];
	result->isnull = &state->isnull[column * PG_BATCH_SIZE];
	result->valid_rows = &state->valid_rows[column];
	result->nwords = 1;
}

static const PgBatchBridgeBatchOps pack_batch_ops = {
	.abi_version = PG_BATCH_BRIDGE_ABI_VERSION,
	.struct_size = sizeof(PgBatchBridgeBatchOps),
	.prepare_columns = pack_prepare_columns,
	.get_datum_column = pack_get_datum_column,
	.get_native_interface = NULL,
	.release = NULL,
};

static void
pack_begin(CustomScanState *node, EState *estate, int eflags)
{
	BatchPackState *state = (BatchPackState *) node;
	PgBatchSlot *slot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);
	List	   *logical_columns = NIL;

	pg_batch_init_children(node, estate, eflags);
	state->child = linitial(node->custom_ps);
	state->ncolumns = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor->natts;
	for (int column = 0; column < state->ncolumns; column++)
		logical_columns = lappend_int(logical_columns, column + 1);
	pg_batch_configure_slot(slot, NULL, logical_columns, 0);
	list_free(logical_columns);
	state->request = pg_batch_bridge->get_request(slot->binding);
}

static Datum
copy_packed_datum(Datum value, Form_pg_attribute attr,
				  MemoryContext context)
{
	MemoryContext oldcontext;
	Datum		result;

	if (attr->attbyval)
		return value;
	oldcontext = MemoryContextSwitchTo(context);
	result = datumCopy(value, false, attr->attlen);
	MemoryContextSwitchTo(oldcontext);
	return result;
}

static int
fill_batch(BatchPackState *state, PgBatchSlot *output)
{
	TupleDesc	desc = output->base.tts_tupleDescriptor;
	MemoryContext context = output->batch_context;
	int			nrows = 0;

	state->values = MemoryContextAlloc(context,
								   sizeof(Datum) * state->ncolumns * PG_BATCH_SIZE);
	state->isnull = MemoryContextAlloc(context,
								   sizeof(bool) * state->ncolumns * PG_BATCH_SIZE);
	state->valid_rows = MemoryContextAllocZero(context,
									 sizeof(uint64) * state->ncolumns);
	while (nrows < PG_BATCH_SIZE)
	{
		TupleTableSlot *slot = ExecProcNode(state->child);

		if (TupIsNull(slot))
			break;
		slot_getsomeattrs(slot, state->ncolumns);
		for (int column = 0; column < state->ncolumns; column++)
		{
			bool		isnull = slot->tts_isnull[column];
			int			offset = column * PG_BATCH_SIZE + nrows;

			state->isnull[offset] = isnull;
			state->values[offset] = isnull ? (Datum) 0 :
				copy_packed_datum(slot->tts_values[column],
								  TupleDescAttr(desc, column), context);
			state->valid_rows[column] |= UINT64CONST(1) << nrows;
		}
		nrows++;
	}
	return nrows;
}

static TupleTableSlot *
pack_exec(CustomScanState *node)
{
	BatchPackState *state = (BatchPackState *) node;
	PgBatchSlot *slot = pg_batch_slot_cast(node->ss.ss_ScanTupleSlot);
	int			nrows;

	if (!state->request->return_batch)
		elog(ERROR, "pg_batch pack requires a batch-aware parent");
	if (state->published)
	{
		if (!state->batch.consumed)
			elog(ERROR, "pg_batch parent requested a new packed batch too early");
		ExecClearTuple(&slot->base);
		state->published = false;
	}
	nrows = fill_batch(state, slot);
	if (nrows == 0)
		return ExecClearTuple(&slot->base);

	state->selection = pg_batch_nrows_mask(nrows);
	state->batch.abi_version = PG_BATCH_BRIDGE_ABI_VERSION;
	state->batch.struct_size = sizeof(PgBatchBridgeBatch);
	state->batch.nrows = nrows;
	state->batch.nwords = 1;
	state->batch.selection = &state->selection;
	state->batch.table_oid = InvalidOid;
	state->batch.ops = &pack_batch_ops;
	state->batch.private_data = state;
	pg_batch_bridge->publish_batch(slot->binding, &state->batch);
	state->published = true;
	state->packed_batches++;
	state->packed_rows += nrows;
	if (node->ss.ps.instrument != NULL)
		node->ss.ps.instrument->tuplecount += nrows - 1;
	return &slot->base;
}

static void
pack_end(CustomScanState *node)
{
	BatchPackState *state = (BatchPackState *) node;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	state->published = false;
	pg_batch_end_children(node);
}

static void
pack_rescan(CustomScanState *node)
{
	BatchPackState *state = (BatchPackState *) node;

	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	state->published = false;
	state->packed_batches = 0;
	state->packed_rows = 0;
	pg_batch_rescan_children(node);
}

static void
pack_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	BatchPackState *state = (BatchPackState *) node;

	ExplainPropertyInteger("Batch Size", NULL, PG_BATCH_SIZE, es);
	if (es->analyze)
	{
		ExplainPropertyInteger("Packed Batches", NULL,
							   state->packed_batches, es);
		ExplainPropertyInteger("Packed Rows", NULL, state->packed_rows, es);
	}
}
