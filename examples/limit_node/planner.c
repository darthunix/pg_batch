#include "postgres.h"

#include "executor/nodeCustom.h"
#include "nodes/makefuncs.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"

#include "internal.h"

static create_upper_paths_hook_type previous_create_upper_paths_hook;

static Plan *plan_limit(PlannerInfo *root, RelOptInfo *rel,
	CustomPath *best_path, List *tlist, List *clauses, List *custom_plans);

static const CustomPathMethods limit_path_methods = {
	.CustomName = "PgBatchLimit",
	.PlanCustomPath = plan_limit,
};

static const CustomScanMethods limit_plan_methods = {
	.CustomName = "PgBatchLimit",
	.CreateCustomScanState = pg_batch_limit_create_state,
};

static bool
supports_path(const Path *path)
{
	return IsA(path, CustomPath) &&
		castNode(CustomPath, path)->methods == &limit_path_methods;
}

static void
get_output_layout(const Plan *plan, PgBatchOutputLayout *result)
{
	const CustomScan *scan;
	List	   *items;
	int		   *columns;
	int			position = 0;

	if (result == NULL || result->struct_size < PG_BATCH_OUTPUT_LAYOUT_MIN_SIZE)
		elog(ERROR, "pg_batch_limit received an incompatible output layout");
	if (!IsA(plan, CustomScan))
		elog(ERROR, "pg_batch_limit expected a custom plan");
	scan = castNode(CustomScan, plan);
	if (scan->methods != &limit_plan_methods ||
		list_length(scan->custom_private) != 2)
		elog(ERROR, "pg_batch_limit received an invalid plan");
	items = lsecond_node(List, scan->custom_private);
	columns = palloc_array(int, list_length(items));
	foreach_int(column, items)
		columns[position++] = column;
	result->ncolumns = position;
	result->batch_columns = columns;
}

const PgBatchProducerOps pg_batch_limit_producer_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_PRODUCER_OPS_ABI_VERSION,
		PgBatchProducerOps),
	.producer_name = PG_BATCH_LIMIT_PRODUCER_NAME,
	.supports_path = supports_path,
	.get_output_layout = get_output_layout,
	.get_request_binding = pg_batch_limit_request_binding,
};

static CustomPath *
make_limit_path(LimitPath *limit, const PgBatchProducerOps *producer)
{
	CustomPath *path = makeNode(CustomPath);

	/* Keep every planner property of the ordinary LimitPath. */
	path->path = limit->path;
	NodeSetTag(path, T_CustomPath);
	path->path.pathtype = T_CustomScan;
	path->flags = 0;
	path->custom_paths = list_make1(limit->subpath);
	path->custom_private =
		list_make3(makeString(pstrdup(producer->producer_name)),
				   copyObject(limit->limitOffset),
				   copyObject(limit->limitCount));
	path->methods = &limit_path_methods;
	return path;
}

static void
create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
				   RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	ListCell   *lc;

	if (previous_create_upper_paths_hook != NULL)
		previous_create_upper_paths_hook(root, stage, input_rel, output_rel,
									 extra);
	if (!pg_batch_limit_enable || stage != UPPERREL_FINAL)
		return;

	foreach(lc, output_rel->pathlist)
	{
		Path	   *candidate = lfirst(lc);
		LimitPath  *limit;
		const PgBatchProducerOps *producer;

		if (!IsA(candidate, LimitPath))
			continue;
		limit = castNode(LimitPath, candidate);
		if (limit->limitOption != LIMIT_OPTION_COUNT)
			continue;
		producer = pg_batch_limit_api->find_producer(limit->subpath);
		if (producer == NULL)
			continue;
		lfirst(lc) = make_limit_path(limit, producer);
	}
}

static Plan *
plan_limit(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
		   List *tlist, List *clauses, List *custom_plans)
{
	const char *producer_name;
	const PgBatchProducerOps *producer;
	PgBatchOutputLayout layout =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchOutputLayout);
	CustomScan *scan;
	Plan	   *child;
	List	   *columns = NIL;

	Assert(clauses == NIL);
	Assert(list_length(custom_plans) == 1);
	Assert(list_length(best_path->custom_private) == 3);
	producer_name = strVal(linitial(best_path->custom_private));
	producer = pg_batch_limit_api->get_producer(producer_name);
	if (producer == NULL)
		elog(ERROR, "pg_batch producer \"%s\" is not registered",
			 producer_name);
	child = linitial(custom_plans);
	producer->get_output_layout(child, &layout);
	if (layout.ncolumns != list_length(child->targetlist) ||
		(layout.ncolumns > 0 && layout.batch_columns == NULL))
		elog(ERROR, "pg_batch producer \"%s\" returned an invalid output layout",
			 producer_name);
	for (int position = 0; position < layout.ncolumns; position++)
		columns = lappend_int(columns, layout.batch_columns[position]);

	scan = makeNode(CustomScan);
	scan->methods = &limit_plan_methods;
	scan->scan.plan.targetlist = copyObject(tlist);
	scan->custom_plans = custom_plans;
	scan->custom_private =
		list_make2(makeString(pstrdup(producer_name)), columns);
	scan->custom_exprs =
		list_make2(copyObject(lsecond(best_path->custom_private)),
				   copyObject(lthird(best_path->custom_private)));
	scan->custom_scan_tlist = copyObject(tlist);
	scan->custom_relids = bms_copy(rel->relids);
	return &scan->scan.plan;
}

void
pg_batch_limit_planner_init(void)
{
	RegisterCustomScanMethods(&limit_plan_methods);
	previous_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = create_upper_paths;
}

void
pg_batch_limit_planner_fini(void)
{
	if (create_upper_paths_hook == create_upper_paths)
		create_upper_paths_hook = previous_create_upper_paths_hook;
}
