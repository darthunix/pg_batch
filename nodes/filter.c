#include "postgres.h"

#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"

#include "internal.h"

typedef struct BatchFilterState
{
	CustomScanState css;
	PgBatchPass *pass;
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
			 PgBatchInputBatch *input_batch, int input_rows)
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
	while ((row = pg_batch_next_selected(batch, row)) >= 0)
	{
		TupleTableSlot *slot;

		ResetExprContext(econtext);
		slot = rows == NULL ? pg_batch_input_select_row(input, row) :
			pg_batch_row_view_select(rows, row);
		econtext->ecxt_scantuple = slot;
		if (!ExecQual(state->css.ss.ps.qual, econtext))
			pg_batch_clear_row(batch, row);
	}
	remaining = pg_batch_row_count(batch);
	InstrCountFiltered1(&state->css.ss, input_rows - remaining);
	return remaining;
}

static const PgBatchPassOps filter_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_PASS_OPS_ABI_VERSION, PgBatchPassOps),
	.process_batch = filter_batch,
};

static void
filter_begin(CustomScanState *node, EState *estate, int eflags)
{
	BatchFilterState *state = (BatchFilterState *) node;
	CustomScan *scan = castNode(CustomScan, node->ss.ps.plan);
	List	   *source_attnums = linitial(scan->custom_private);
	int			nfilter = intVal(lsecond(scan->custom_private));
	Bitmapset  *filter_columns = NULL;
	Bitmapset  *project_columns =
		intlist_to_bitmap(lthird(scan->custom_private));
	PgBatchLayout layout = PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);
	int		   *target_columns;
	int			target = 0;

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
	state->pass = pg_batch_pass_create(estate->es_query_cxt, node, estate,
		eflags, PG_BATCH_PRODUCER_NAME, &layout, filter_columns,
		project_columns, 0, PG_BATCH_PASS_INPUT_ROW, &filter_ops, state);
	bms_free(filter_columns);
	bms_free(project_columns);
	pfree(target_columns);
}

static TupleTableSlot *
filter_exec(CustomScanState *node)
{
	return pg_batch_pass_exec(((BatchFilterState *) node)->pass);
}

static void
filter_end(CustomScanState *node)
{
	pg_batch_pass_end(((BatchFilterState *) node)->pass);
}

static void
filter_rescan(CustomScanState *node)
{
	pg_batch_pass_rescan(((BatchFilterState *) node)->pass);
}

static void
filter_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	BatchFilterState *state = (BatchFilterState *) node;
	const PgBatchPassStats *stats = pg_batch_pass_stats(state->pass);

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
