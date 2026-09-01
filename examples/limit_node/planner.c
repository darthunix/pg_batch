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

#define LIMIT_PATH_DATA_KIND "PgBatchLimitPath"
#define LIMIT_PLAN_DATA_KIND "PgBatchLimit"
#define LIMIT_PLAN_DATA_VERSION 1

static List *
make_limit_path_data(const char *node_name, const Node *offset,
					 const Node *count)
{
	PgBatchPlanWriter *writer = pg_batch_plan_writer_create(
		LIMIT_PATH_DATA_KIND, LIMIT_PLAN_DATA_VERSION);

	pg_batch_plan_write_string(writer, "node", node_name);
	pg_batch_plan_write_node(writer, "offset", offset);
	pg_batch_plan_write_node(writer, "count", count);
	return pg_batch_plan_writer_finish(writer);
}

void
pg_batch_limit_read_plan(const CustomScan *scan,
						 PgBatchLimitPlanData *result)
{
	PgBatchPlanReader *reader = pg_batch_plan_reader_create(
		scan->custom_private, LIMIT_PLAN_DATA_KIND, LIMIT_PLAN_DATA_VERSION);

	result->node_name = pg_batch_plan_read_string(reader, "node");
	result->output_columns = pg_batch_plan_read_int_list(reader,
		"output_columns");
	result->column_count = pg_batch_plan_read_int(reader, "column_count");
	pg_batch_plan_reader_finish(reader);
}

static bool
supports_path(const Path *path)
{
	return IsA(path, CustomPath) &&
		((const CustomPath *) path)->methods == &limit_path_methods;
}

static void
get_layout(const Plan *plan, PgBatchLayout *result)
{
	const CustomScan *scan;
	PgBatchLimitPlanData data;
	int		   *columns;
	int			position = 0;

	if (result == NULL || result->struct_size < PG_BATCH_LAYOUT_MIN_SIZE)
		elog(ERROR, "pg_batch_limit received an incompatible output layout");
	if (!IsA(plan, CustomScan))
		elog(ERROR, "pg_batch_limit expected a custom plan");
	scan = (const CustomScan *) plan;
	if (scan->methods != &limit_plan_methods)
		elog(ERROR, "pg_batch_limit received an invalid plan");
	pg_batch_limit_read_plan(scan, &data);
	columns = palloc_array(int, list_length(data.output_columns));
	foreach_int(column, data.output_columns)
		columns[position++] = column;
	result->ncolumns = data.column_count;
	result->ntargets = position;
	result->target_columns = columns;
}

const PgBatchNodeOps pg_batch_limit_node_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_NODE_OPS_ABI_VERSION,
		PgBatchNodeOps),
	.node_name = PG_BATCH_LIMIT_NODE_NAME,
	.supports_path = supports_path,
	.get_layout = get_layout,
	.get_request_binding = pg_batch_limit_request_binding,
};

static CustomPath *
make_limit_path(LimitPath *limit, const PgBatchNodeOps *node)
{
	CustomPath *path = makeNode(CustomPath);

	/* Keep every planner property of the ordinary LimitPath. */
	path->path = limit->path;
	NodeSetTag(path, T_CustomPath);
	path->path.pathtype = T_CustomScan;
	path->flags = 0;
	path->custom_paths = list_make1(limit->subpath);
	path->custom_private = make_limit_path_data(node->node_name,
		limit->limitOffset, limit->limitCount);
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
		const PgBatchNodeOps *node;

		if (!IsA(candidate, LimitPath))
			continue;
		limit = castNode(LimitPath, candidate);
		if (limit->limitOption != LIMIT_OPTION_COUNT)
			continue;
		node = pg_batch_limit_api->find_node(limit->subpath);
		if (node == NULL)
			continue;
		lfirst(lc) = make_limit_path(limit, node);
	}
}

static Plan *
plan_limit(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
		   List *tlist, List *clauses, List *custom_plans)
{
	const char *node_name;
	const PgBatchNodeOps *node;
	PgBatchPlanReader *path_data;
	PgBatchPlanWriter *plan_data;
	PgBatchLayout layout = PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);
	CustomScan *scan;
	Plan	   *child;
	List	   *columns = NIL;
	Node	   *offset;
	Node	   *count;

	Assert(clauses == NIL);
	Assert(list_length(custom_plans) == 1);
	path_data = pg_batch_plan_reader_create(best_path->custom_private,
		LIMIT_PATH_DATA_KIND, LIMIT_PLAN_DATA_VERSION);
	node_name = pg_batch_plan_read_string(path_data, "node");
	offset = pg_batch_plan_read_node(path_data, "offset");
	count = pg_batch_plan_read_node(path_data, "count");
	pg_batch_plan_reader_finish(path_data);
	node = pg_batch_limit_api->get_node(node_name);
	if (node == NULL)
		elog(ERROR, "pg_batch node \"%s\" is not registered",
			 node_name);
	child = linitial(custom_plans);
	node->get_layout(child, &layout);
	pg_batch_check_layout(&layout);
	if (layout.ntargets != list_length(child->targetlist))
		elog(ERROR, "pg_batch node \"%s\" returned an invalid output layout",
			 node_name);
	for (int position = 0; position < layout.ntargets; position++)
		columns = lappend_int(columns,
			pg_batch_layout_column(&layout, position));

	scan = makeNode(CustomScan);
	scan->methods = &limit_plan_methods;
	scan->scan.plan.targetlist = copyObject(tlist);
	scan->custom_plans = custom_plans;
	plan_data = pg_batch_plan_writer_create(LIMIT_PLAN_DATA_KIND,
		LIMIT_PLAN_DATA_VERSION);
	pg_batch_plan_write_string(plan_data, "node", node_name);
	pg_batch_plan_write_int_list(plan_data, "output_columns", columns);
	pg_batch_plan_write_int(plan_data, "column_count", layout.ncolumns);
	scan->custom_private = pg_batch_plan_writer_finish(plan_data);
	scan->custom_exprs =
		list_make2(copyObject(offset), copyObject(count));
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
