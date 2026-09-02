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
	return pg_batch_path_matches(path, &limit_path_methods);
}

const PgBatchNodeOps pg_batch_limit_node_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_NODE_OPS_ABI_VERSION,
		PgBatchNodeOps),
	.node_name = PG_BATCH_LIMIT_NODE_NAME,
	.supports_path = supports_path,
	.get_layout = pg_batch_plan_get_layout,
	.get_request_binding = pg_batch_limit_request_binding,
};

static CustomPath *
make_limit_path(LimitPath *limit)
{
	PgBatchPathConfig config =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPathConfig);

	config.template_path = &limit->path;
	config.methods = &limit_path_methods;
	config.children = list_make1(limit->subpath);
	config.expressions = list_make2(limit->limitOffset, limit->limitCount);
	return pg_batch_path_create(&config);
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
		lfirst(lc) = make_limit_path(limit);
	}
}

static Plan *
plan_limit(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
		   List *tlist, List *clauses, List *custom_plans)
{
	PgBatchPathInfo path_info =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPathInfo);
	PgBatchPlanConfig config =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanConfig);

	Assert(clauses == NIL);
	Assert(list_length(custom_plans) == 1);
	pg_batch_path_get_info(best_path, &path_info);
	config.api = pg_batch_limit_api;
	config.methods = &limit_plan_methods;
	config.layout_policy = PG_BATCH_LAYOUT_PRESERVE_CHILD;
	config.layout_child = 0;
	config.expressions = path_info.expressions;
	return pg_batch_plan_create(rel, best_path, tlist, custom_plans,
		&config);
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
