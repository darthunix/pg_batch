#include "postgres.h"

#include "executor/executor.h"
#include "executor/instrument.h"
#include "executor/nodeCustom.h"

#include "runtime.h"

struct PgBatchPass
{
	const PgBatchAPI *api;
	CustomScanState *node;
	PlanState  *child;
	PgBatchInput *input;
	PgBatchBinding *binding;
	const PgBatchRequest *request;
	const PgBatchPassOps *ops;
	void	   *private_data;
	Bitmapset  *filter_columns;
	Bitmapset  *project_columns;
	int			max_rows;
	int		   *batch_columns;
	PgBatchPassRowMode row_mode;
	TupleTableSlot *active_slot;
	PgBatch    *active_batch;
	int			next_row;
	bool		request_forwarded;
	bool		stopped;
	PgBatchPassStats stats;
};

static void
reject_batch(TupleTableSlot *slot, PgBatch *batch)
{
	(void) slot;
	(void) batch;
	elog(ERROR, "pg_batch pass-through request slot cannot accept a batch");
}

static void
reject_row(TupleTableSlot *slot, PgBatch *batch, int row)
{
	(void) slot;
	(void) batch;
	(void) row;
	elog(ERROR, "pg_batch pass-through request slot has no batch rows");
}

static const PgBatchConsumerOps request_consumer_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_CONSUMER_OPS_ABI_VERSION,
		PgBatchConsumerOps),
	.accept_batch = reject_batch,
	.select_row = reject_row,
};

PgBatchPass *
pg_batch_pass_create(MemoryContext parent_context, CustomScanState *node,
					 EState *estate, int eflags, const char *producer_name,
					 const PgBatchLayout *layout,
					 const Bitmapset *filter_columns,
					 const Bitmapset *project_columns, int max_rows,
					 PgBatchPassRowMode row_mode, const PgBatchPassOps *ops,
					 void *private_data)
{
	CustomScan *scan;
	PgBatchPass *pass;
	MemoryContext oldcontext;

	if (parent_context == NULL || node == NULL || estate == NULL ||
		producer_name == NULL || layout == NULL || max_rows < 0 || ops == NULL ||
		ops->abi_version != PG_BATCH_PASS_OPS_ABI_VERSION ||
		ops->struct_size < PG_BATCH_PASS_OPS_MIN_SIZE)
		elog(ERROR, "invalid pg_batch pass-through configuration");
	if (row_mode == PG_BATCH_PASS_COPY_ROW &&
		layout->ntargets != node->ss.ss_ScanTupleSlot->tts_tupleDescriptor->natts)
		elog(ERROR, "pg_batch copied row layout does not match its scan slot");
	scan = castNode(CustomScan, node->ss.ps.plan);
	if (list_length(scan->custom_plans) != 1)
		elog(ERROR, "pg_batch pass-through node requires one child");

	pass = MemoryContextAllocZero(parent_context, sizeof(*pass));
	pass->api = pg_batch_api_get();
	pass->node = node;
	pass->ops = ops;
	pass->private_data = private_data;
	pass->row_mode = row_mode;
	oldcontext = MemoryContextSwitchTo(parent_context);
	pass->filter_columns = bms_copy(filter_columns);
	pass->project_columns = bms_copy(project_columns);
	MemoryContextSwitchTo(oldcontext);
	pass->max_rows = max_rows;
	pass->next_row = -1;
	pass->binding = pass->api->attach_slot(node->ss.ss_ScanTupleSlot,
		layout, &request_consumer_ops);
	pass->api->set_request(pass->binding, NULL, NULL, false, 0);
	pass->request = pass->api->get_request(pass->binding);
	if (row_mode == PG_BATCH_PASS_COPY_ROW && layout->ntargets > 0)
	{
		pass->batch_columns = MemoryContextAlloc(parent_context,
			sizeof(int) * layout->ntargets);
		for (int target = 0; target < layout->ntargets; target++)
			pass->batch_columns[target] =
				pg_batch_layout_column(layout, target);
	}
	pass->child = ExecInitNode(linitial(scan->custom_plans), estate, eflags);
	node->custom_ps = list_make1(pass->child);
	pass->input = pg_batch_input_create(parent_context, pass->child,
		producer_name);
	return pass;
}

PgBatchBinding *
pg_batch_pass_binding(PgBatchPass *pass)
{
	return pass->binding;
}

PlanState *
pg_batch_pass_child(PgBatchPass *pass)
{
	return pass->child;
}

const PgBatchPassStats *
pg_batch_pass_stats(PgBatchPass *pass)
{
	return &pass->stats;
}

void
pg_batch_pass_stop(PgBatchPass *pass)
{
	pass->stopped = true;
}

static void
forward_request(PgBatchPass *pass)
{
	const PgBatchRequest *child = pg_batch_input_request(pass->input);
	Bitmapset  *filters;
	Bitmapset  *projects;
	int			max_rows;

	pass->request = pass->api->seal_request(pass->binding);
	if (child->layout->ncolumns != pass->request->layout->ncolumns)
		elog(ERROR, "pg_batch cannot pass through %d columns as %d columns",
			 child->layout->ncolumns, pass->request->layout->ncolumns);
	filters = bms_union(child->filter_columns, pass->filter_columns);
	filters = bms_add_members(filters, pass->request->filter_columns);
	projects = bms_union(child->project_columns, pass->project_columns);
	projects = bms_add_members(projects, pass->request->project_columns);
	max_rows = child->max_rows;
	if (pass->max_rows > 0)
		max_rows = max_rows == 0 ? pass->max_rows :
			Min(max_rows, pass->max_rows);
	if (pass->request->max_rows > 0)
		max_rows = max_rows == 0 ? pass->request->max_rows :
			Min(max_rows, pass->request->max_rows);
	/* This executor always consumes the child as complete batches. */
	pg_batch_input_set_request(pass->input, filters, projects, true, max_rows);
	bms_free(filters);
	bms_free(projects);
	pass->request_forwarded = true;
}

static bool
fetch_batch(PgBatchPass *pass)
{
	for (;;)
	{
		PgBatchInputBatch input;
		int			rows;

		if (pass->stopped)
			return false;
		if (!pg_batch_input_next(pass->input, &input))
			return false;
		rows = pg_batch_row_count(input.batch);
		pass->stats.input_batches++;
		pass->stats.input_rows += rows;
		if (pass->ops->process_batch != NULL)
			rows = pass->ops->process_batch(pass->private_data, pass->input,
				&input, rows);
		Assert(rows == pg_batch_row_count(input.batch));
		if (rows == 0)
		{
			pg_batch_input_finish(pass->input);
			continue;
		}
		if (!pass->request->return_batch)
		{
			const PgBatchRequest *child =
				pg_batch_input_request(pass->input);

			pg_batch_materialize_columns(input.batch,
				child->project_columns, input.batch->selection,
				PG_BATCH_COLUMN_PROJECT);
		}
		pass->stats.output_rows += rows;
		pass->active_slot = input.slot;
		pass->active_batch = input.batch;
		pass->next_row = pg_batch_next_selected(input.batch, -1);
		return true;
	}
}

static TupleTableSlot *
empty_result(PgBatchPass *pass)
{
	PlanState  *planstate = &pass->node->ss.ps;

	if (planstate->ps_ProjInfo != NULL)
		return ExecClearTuple(planstate->ps_ProjInfo->pi_state.resultslot);
	return ExecClearTuple(pass->node->ss.ss_ScanTupleSlot);
}

static TupleTableSlot *
exec_batch(PgBatchPass *pass)
{
	int			rows;

	pass->active_slot = NULL;
	pass->active_batch = NULL;
	if (!fetch_batch(pass))
		return empty_result(pass);
	pg_batch_input_select_row(pass->input, pass->next_row);
	rows = pg_batch_row_count(pass->active_batch);
	if (pass->node->ss.ps.instrument != NULL)
		pass->node->ss.ps.instrument->tuplecount += rows - 1;
	return pass->active_slot;
}

static TupleTableSlot *
exec_row(PgBatchPass *pass)
{
	PlanState  *planstate = &pass->node->ss.ps;

	for (;;)
	{
		TupleTableSlot *selected;
		int			row;

		if (pass->active_batch == NULL && !fetch_batch(pass))
			return empty_result(pass);
		if (pass->next_row < 0)
		{
			pg_batch_input_finish(pass->input);
			pass->active_slot = NULL;
			pass->active_batch = NULL;
			continue;
		}
		row = pass->next_row;
		pass->next_row = pg_batch_next_selected(pass->active_batch, row);
		if (pass->row_mode == PG_BATCH_PASS_COPY_ROW)
			return pg_batch_input_copy_row(pass->input, row,
				pass->node->ss.ss_ScanTupleSlot, pass->batch_columns);
		selected = pg_batch_input_select_row(pass->input, row);
		if (planstate->ps_ProjInfo == NULL)
			return selected;
		ResetExprContext(planstate->ps_ExprContext);
		planstate->ps_ExprContext->ecxt_scantuple = selected;
		return ExecProject(planstate->ps_ProjInfo);
	}
}

TupleTableSlot *
pg_batch_pass_exec(PgBatchPass *pass)
{
	if (!pass->request_forwarded)
		forward_request(pass);
	return pass->request->return_batch ? exec_batch(pass) : exec_row(pass);
}

void
pg_batch_pass_end(PgBatchPass *pass)
{
	ExecEndNode(pass->child);
}

void
pg_batch_pass_rescan(PgBatchPass *pass)
{
	ExecReScan(pass->child);
	pg_batch_input_rescan(pass->input);
	pass->active_slot = NULL;
	pass->active_batch = NULL;
	pass->next_row = -1;
	pass->stopped = false;
	MemSet(&pass->stats, 0, sizeof(pass->stats));
}
