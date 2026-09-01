#include "postgres.h"

#include "executor/executor.h"
#include "executor/instrument.h"
#include "executor/nodeCustom.h"

#include "runtime.h"

struct PgBatchUnary
{
	const PgBatchAPI *api;
	CustomScanState *node;
	PlanState  *child;
	PgBatchInput *input;
	PgBatchBinding *binding;
	const PgBatchRequest *request;
	PgBatchUnaryProcess process;
	void	   *private_data;
	Bitmapset  *filter_columns;
	Bitmapset  *project_columns;
	int			max_rows;
	int		   *batch_columns;
	PgBatchUnaryRowMode row_mode;
	TupleTableSlot *active_slot;
	PgBatch    *active_batch;
	int			next_row;
	bool		request_forwarded;
	bool		stopped;
	PgBatchUnaryStats stats;
};

static void
reject_batch(void *consumer_state, TupleTableSlot *slot, PgBatch *batch)
{
	(void) consumer_state;
	(void) slot;
	(void) batch;
	elog(ERROR, "pg_batch unary request slot cannot accept a batch");
}

static void
reject_row(void *consumer_state, TupleTableSlot *slot, PgBatch *batch, int row)
{
	(void) consumer_state;
	(void) slot;
	(void) batch;
	(void) row;
	elog(ERROR, "pg_batch unary request slot has no batch rows");
}

static const PgBatchConsumerOps request_consumer_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_CONSUMER_OPS_ABI_VERSION,
		PgBatchConsumerOps),
	.accept_batch = reject_batch,
	.select_row = reject_row,
};

PgBatchUnary *
pg_batch_unary_create(const PgBatchUnaryConfig *config)
{
	CustomScan *scan;
	PgBatchUnary *unary;
	MemoryContext oldcontext;

	if (config == NULL ||
		config->struct_size < PG_BATCH_UNARY_CONFIG_MIN_SIZE ||
		config->parent_context == NULL || config->node == NULL ||
		config->estate == NULL || config->child_name == NULL ||
		config->layout == NULL || config->max_rows < 0)
		elog(ERROR, "invalid pg_batch unary configuration");
	if (config->row_mode == PG_BATCH_UNARY_COPY_ROW &&
		config->layout->ntargets !=
		config->node->ss.ss_ScanTupleSlot->tts_tupleDescriptor->natts)
		elog(ERROR, "pg_batch copied row layout does not match its scan slot");
	scan = castNode(CustomScan, config->node->ss.ps.plan);
	if (list_length(scan->custom_plans) != 1)
		elog(ERROR, "pg_batch unary node requires one child");

	unary = MemoryContextAllocZero(config->parent_context, sizeof(*unary));
	unary->api = pg_batch_api_get();
	unary->node = config->node;
	unary->process = config->process;
	unary->private_data = config->private_data;
	unary->row_mode = config->row_mode;
	oldcontext = MemoryContextSwitchTo(config->parent_context);
	unary->filter_columns = bms_copy(config->filter_columns);
	unary->project_columns = bms_copy(config->project_columns);
	MemoryContextSwitchTo(oldcontext);
	unary->max_rows = config->max_rows;
	unary->next_row = -1;
	unary->binding = unary->api->attach_slot(
		config->node->ss.ss_ScanTupleSlot, config->layout,
		&request_consumer_ops, unary);
	unary->api->set_request(unary->binding,
		&(PgBatchRequestSpec) {
			.struct_size = sizeof(PgBatchRequestSpec),
			.output_mode = PG_BATCH_OUTPUT_ROWS
		});
	unary->request = unary->api->get_request(unary->binding);
	if (config->row_mode == PG_BATCH_UNARY_COPY_ROW &&
		config->layout->ntargets > 0)
	{
		unary->batch_columns = MemoryContextAlloc(config->parent_context,
			sizeof(int) * config->layout->ntargets);
		for (int target = 0; target < config->layout->ntargets; target++)
		{
			unary->batch_columns[target] =
				pg_batch_layout_column(config->layout, target);
			if (unary->batch_columns[target] < 0)
				elog(ERROR, "pg_batch copied target %d has no batch column",
					 target + 1);
		}
	}
	unary->child = ExecInitNode(linitial(scan->custom_plans), config->estate,
		config->eflags);
	config->node->custom_ps = list_make1(unary->child);
	unary->input = pg_batch_input_create(config->parent_context, unary->child,
		config->child_name);
	return unary;
}

PgBatchBinding *
pg_batch_unary_binding(PgBatchUnary *unary)
{
	return unary->binding;
}

PlanState *
pg_batch_unary_child(PgBatchUnary *unary)
{
	return unary->child;
}

const PgBatchUnaryStats *
pg_batch_unary_stats(PgBatchUnary *unary)
{
	return &unary->stats;
}

void
pg_batch_unary_stop(PgBatchUnary *unary)
{
	unary->stopped = true;
}

static void
forward_request(PgBatchUnary *unary)
{
	const PgBatchRequest *child = pg_batch_input_request(unary->input);
	Bitmapset  *filters;
	Bitmapset  *projects;
	int			max_rows;

	unary->request = unary->api->seal_request(unary->binding);
	if (child->layout->ncolumns != unary->request->layout->ncolumns)
		elog(ERROR, "pg_batch cannot unary through %d columns as %d columns",
			 child->layout->ncolumns, unary->request->layout->ncolumns);
	filters = bms_union(child->spec.filter_columns, unary->filter_columns);
	filters = bms_add_members(filters, unary->request->spec.filter_columns);
	projects = bms_union(child->spec.project_columns, unary->project_columns);
	projects = bms_add_members(projects, unary->request->spec.project_columns);
	max_rows = child->spec.max_rows;
	if (unary->max_rows > 0)
		max_rows = max_rows == 0 ? unary->max_rows :
			Min(max_rows, unary->max_rows);
	if (unary->request->spec.max_rows > 0)
		max_rows = max_rows == 0 ? unary->request->spec.max_rows :
			Min(max_rows, unary->request->spec.max_rows);
	/* This executor always consumes the child as complete batches. */
	pg_batch_input_set_request(unary->input,
		&(PgBatchRequestSpec) {
			.struct_size = sizeof(PgBatchRequestSpec),
			.filter_columns = filters,
			.project_columns = projects,
			.output_mode = PG_BATCH_OUTPUT_BATCH,
			.max_rows = max_rows
		});
	bms_free(filters);
	bms_free(projects);
	unary->request_forwarded = true;
}

static bool
fetch_batch(PgBatchUnary *unary)
{
	for (;;)
	{
		PgBatchInputResult input;
		int			rows;

		if (unary->stopped)
			return false;
		if (!pg_batch_input_next(unary->input, &input))
			return false;
		rows = pg_batch_row_count(input.batch);
		unary->stats.input_batches++;
		unary->stats.input_rows += rows;
		if (unary->process != NULL)
			rows = unary->process(unary->private_data, unary->input,
				&input, rows);
		Assert(rows == pg_batch_row_count(input.batch));
		if (rows == 0)
		{
			pg_batch_input_finish(unary->input);
			continue;
		}
		if (unary->request->spec.output_mode == PG_BATCH_OUTPUT_ROWS)
		{
			const PgBatchRequest *child =
				pg_batch_input_request(unary->input);

			pg_batch_materialize_columns(input.batch,
				child->spec.project_columns, &input.batch->selection,
				PG_BATCH_COLUMN_PROJECT);
		}
		unary->stats.output_rows += rows;
		unary->active_slot = input.slot;
		unary->active_batch = input.batch;
		unary->next_row = pg_batch_selection_next(&input.batch->selection, -1);
		return true;
	}
}

static TupleTableSlot *
empty_result(PgBatchUnary *unary)
{
	PlanState  *planstate = &unary->node->ss.ps;

	if (planstate->ps_ProjInfo != NULL)
		return ExecClearTuple(planstate->ps_ProjInfo->pi_state.resultslot);
	return ExecClearTuple(unary->node->ss.ss_ScanTupleSlot);
}

static TupleTableSlot *
exec_batch(PgBatchUnary *unary)
{
	int			rows;

	unary->active_slot = NULL;
	unary->active_batch = NULL;
	if (!fetch_batch(unary))
		return empty_result(unary);
	pg_batch_input_select_row(unary->input, unary->next_row);
	rows = pg_batch_row_count(unary->active_batch);
	if (unary->node->ss.ps.instrument != NULL)
		unary->node->ss.ps.instrument->tuplecount += rows - 1;
	return unary->active_slot;
}

static TupleTableSlot *
exec_row(PgBatchUnary *unary)
{
	PlanState  *planstate = &unary->node->ss.ps;

	for (;;)
	{
		TupleTableSlot *selected;
		int			row;

		if (unary->active_batch == NULL && !fetch_batch(unary))
			return empty_result(unary);
		if (unary->next_row < 0)
		{
			pg_batch_input_finish(unary->input);
			unary->active_slot = NULL;
			unary->active_batch = NULL;
			continue;
		}
		row = unary->next_row;
		unary->next_row = pg_batch_selection_next(&unary->active_batch->selection, row);
		if (unary->row_mode == PG_BATCH_UNARY_COPY_ROW)
			return pg_batch_input_copy_row(unary->input, row,
				unary->node->ss.ss_ScanTupleSlot, unary->batch_columns);
		selected = pg_batch_input_select_row(unary->input, row);
		if (planstate->ps_ProjInfo == NULL)
			return selected;
		ResetExprContext(planstate->ps_ExprContext);
		planstate->ps_ExprContext->ecxt_scantuple = selected;
		return ExecProject(planstate->ps_ProjInfo);
	}
}

TupleTableSlot *
pg_batch_unary_exec(PgBatchUnary *unary)
{
	if (!unary->request_forwarded)
		forward_request(unary);
	return unary->request->spec.output_mode == PG_BATCH_OUTPUT_BATCH ?
		exec_batch(unary) : exec_row(unary);
}

void
pg_batch_unary_end(PgBatchUnary *unary)
{
	ExecEndNode(unary->child);
}

void
pg_batch_unary_rescan(PgBatchUnary *unary)
{
	ExecReScan(unary->child);
	pg_batch_input_rescan(unary->input);
	unary->active_slot = NULL;
	unary->active_batch = NULL;
	unary->next_row = -1;
	unary->stopped = false;
	MemSet(&unary->stats, 0, sizeof(unary->stats));
}
