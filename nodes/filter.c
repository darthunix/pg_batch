#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"

#include "internal.h"

typedef struct BatchFilterState
{
	CustomScanState css;
	PgBatchUnary *unary;
} BatchFilterState;

static const CustomExecMethods pg_batch_filter_exec_methods;

Node *
pg_batch_create_filter_state(CustomScan *cscan)
{
	BatchFilterState *state = palloc0_object(BatchFilterState);

	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &pg_batch_filter_exec_methods;
	state->css.slotOps = &PgBatchSlotOps;
	return (Node *) state;
}

static Bitmapset *
intlist_to_bitmap(List *items)
{
	Bitmapset  *result = NULL;

	foreach_int(item, items)
		result = bms_add_member(result, item);
	return result;
}

static int
filter_batch(void *private_data, PgBatchInput *input,
			 PgBatchInputResult *input_batch, int input_rows)
{
	BatchFilterState *state = private_data;
	ExprContext *econtext = state->css.ss.ps.ps_ExprContext;
	PgBatch    *batch = input_batch->batch;
	const PgBatchRowView *rows;
	int			remaining;
	int			row = -1;

	if (state->css.ss.ps.qual == NULL)
		return input_rows;
	rows = pg_batch_input_row_view(input);
	while ((row = pg_batch_selection_next(&batch->selection, row)) >= 0)
	{
		TupleTableSlot *slot;

		ResetExprContext(econtext);
		slot = rows == NULL ? pg_batch_input_select_row(input, row) :
			pg_batch_row_view_select(rows, row);
		econtext->ecxt_scantuple = slot;
		if (!ExecQual(state->css.ss.ps.qual, econtext))
			pg_batch_selection_clear(&batch->selection, row);
	}
	remaining = pg_batch_row_count(batch);
	InstrCountFiltered1(&state->css.ss, input_rows - remaining);
	return remaining;
}

static void
filter_begin(CustomScanState *node, EState *estate, int eflags)
{
	BatchFilterState *state = (BatchFilterState *) node;
	CustomScan *scan = castNode(CustomScan, node->ss.ps.plan);
	PgBatchFilterPlanData data;
	List	   *source_attnums;
	int			nfilter;
	Bitmapset  *filter_columns = NULL;
	Bitmapset  *project_columns;
	PgBatchLayout layout = PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);
	PgBatchUnaryConfig config;
	int		   *target_columns;
	int			target = 0;

	pg_batch_read_filter_plan(scan, &data);
	source_attnums = data.source_attnums;
	nfilter = data.nfilter_columns;
	project_columns = intlist_to_bitmap(data.project_columns);

	layout.ncolumns = list_length(source_attnums);
	layout.ntargets = list_length(node->ss.ps.plan->targetlist);
	target_columns = palloc_array(int, layout.ntargets);
	foreach_ptr(TargetEntry, tle, node->ss.ps.plan->targetlist)
	{
		Node	   *expr = pg_batch_strip_relabel((Node *) tle->expr);

		target_columns[target++] = IsA(expr, Var) &&
			castNode(Var, expr)->varno == OUTER_VAR ?
			castNode(Var, expr)->varattno - 1 : -1;
	}
	layout.target_columns = target_columns;
	if (nfilter > 0)
		filter_columns = bms_add_range(filter_columns, 0, nfilter - 1);
	config = (PgBatchUnaryConfig) {
		.struct_size = sizeof(config),
		.parent_context = estate->es_query_cxt,
		.node = node,
		.estate = estate,
		.eflags = eflags,
		.child_name = PG_BATCH_NODE_NAME,
		.layout = &layout,
		.filter_columns = filter_columns,
		.project_columns = project_columns,
		.row_mode = PG_BATCH_UNARY_INPUT_ROW,
		.process = filter_batch,
		.private_data = state,
	};
	state->unary = pg_batch_unary_create(&config);
	bms_free(filter_columns);
	bms_free(project_columns);
	pfree(target_columns);
}

static TupleTableSlot *
filter_exec(CustomScanState *node)
{
	return pg_batch_unary_exec(((BatchFilterState *) node)->unary);
}

static void
filter_end(CustomScanState *node)
{
	pg_batch_unary_end(((BatchFilterState *) node)->unary);
}

static void
filter_rescan(CustomScanState *node)
{
	pg_batch_unary_rescan(((BatchFilterState *) node)->unary);
}

static void
filter_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	BatchFilterState *state = (BatchFilterState *) node;
	const PgBatchUnaryStats *stats = pg_batch_unary_stats(state->unary);

	if (es->analyze)
	{
		ExplainPropertyInteger("Input Batches", NULL, stats->input_batches, es);
		ExplainPropertyInteger("Input Rows", NULL, stats->input_rows, es);
		ExplainPropertyInteger("Output Rows", NULL, stats->output_rows, es);
	}
}

static const CustomExecMethods pg_batch_filter_exec_methods = {
	.CustomName = "PgBatchFilterProject",
	.BeginCustomScan = filter_begin,
	.ExecCustomScan = filter_exec,
	.EndCustomScan = filter_end,
	.ReScanCustomScan = filter_rescan,
	.ExplainCustomScan = filter_explain,
};
