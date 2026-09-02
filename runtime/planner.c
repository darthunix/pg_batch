#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/value.h"

#include "plan.h"
#include "planner.h"

#define PATH_DATA_KIND "pg_batch.path"
#define PLAN_DATA_KIND "pg_batch.plan"
#define COMMON_DATA_VERSION 1

static void check_path_config(const PgBatchPathConfig *config);
static void check_plan_config(const PgBatchPlanConfig *config);
static void check_api(const PgBatchAPI *api);
static void check_plan_child_lists(const CustomPath *path,
	const List *child_plans);
static void copy_layout(const PgBatchLayout *source, PgBatchLayout *result);

static void
check_api(const PgBatchAPI *api)
{
	if (api == NULL || api->abi_version != PG_BATCH_API_ABI_VERSION ||
		api->struct_size < PG_BATCH_API_MIN_SIZE || api->find_node == NULL)
		elog(ERROR, "pg_batch plan received an incompatible bridge API");
}

static void
check_path_config(const PgBatchPathConfig *config)
{
	if (config == NULL ||
		config->struct_size < PG_BATCH_PATH_CONFIG_MIN_SIZE)
		elog(ERROR, "pg_batch received an incompatible path configuration");
	if (config->template_path == NULL || config->methods == NULL)
		elog(ERROR, "pg_batch path requires a template and methods");
	foreach_ptr(Path, child, config->children)
	{
		if (child == NULL)
			elog(ERROR, "pg_batch path received an invalid child");
	}
}

static void
check_plan_config(const PgBatchPlanConfig *config)
{
	if (config == NULL ||
		config->struct_size < PG_BATCH_PLAN_CONFIG_MIN_SIZE)
		elog(ERROR, "pg_batch received an incompatible plan configuration");
	if (config->methods == NULL)
		elog(ERROR, "pg_batch plan requires scan methods");
	check_api(config->api);
	if (config->layout_policy < PG_BATCH_LAYOUT_PRESERVE_CHILD ||
		config->layout_policy > PG_BATCH_LAYOUT_EXPLICIT)
		elog(ERROR, "pg_batch plan received an invalid layout policy");
	if (config->layout_policy == PG_BATCH_LAYOUT_EXPLICIT &&
		config->explicit_layout == NULL)
		elog(ERROR, "pg_batch plan requires an explicit output layout");
}

static void
check_plan_child_lists(const CustomPath *path, const List *child_plans)
{
	if (path == NULL || !IsA(path, CustomPath))
		elog(ERROR, "pg_batch expected a custom path");
	if (list_length(path->custom_paths) != list_length(child_plans))
		elog(ERROR, "pg_batch path and plan child counts differ");
}

static void
copy_layout(const PgBatchLayout *source, PgBatchLayout *result)
{
	pg_batch_check_layout(source);
	result->ncolumns = source->ncolumns;
	result->ntargets = source->ntargets;
	if (source->target_columns == NULL)
		result->target_columns = NULL;
	else
	{
		int		   *columns = palloc_array(int, source->ntargets);

		memcpy(columns, source->target_columns,
			   sizeof(*columns) * source->ntargets);
		result->target_columns = columns;
	}
}

CustomPath *
pg_batch_path_create(const PgBatchPathConfig *config)
{
	PgBatchPlanWriter *writer;
	CustomPath *path;

	check_path_config(config);
	path = makeNode(CustomPath);
	path->path = *config->template_path;
	NodeSetTag(path, T_CustomPath);
	path->path.pathtype = T_CustomScan;
	path->flags = config->flags;
	path->custom_paths = list_copy(config->children);
	path->custom_restrictinfo = copyObject(config->restrictinfo);
	writer = pg_batch_plan_writer_create(PATH_DATA_KIND,
		COMMON_DATA_VERSION);
	pg_batch_plan_write_list(writer, "expressions", config->expressions);
	pg_batch_plan_write_node(writer, "node_data", config->node_data);
	path->custom_private = pg_batch_plan_writer_finish(writer);
	path->methods = config->methods;
	return path;
}

bool
pg_batch_path_matches(const Path *path, const CustomPathMethods *methods)
{
	return path != NULL && methods != NULL && IsA(path, CustomPath) &&
		((const CustomPath *) path)->methods == methods;
}

void
pg_batch_path_get_info(const CustomPath *path, PgBatchPathInfo *result)
{
	PgBatchPlanReader *reader;

	if (result == NULL || result->struct_size < PG_BATCH_PATH_INFO_MIN_SIZE)
		elog(ERROR, "pg_batch received an incompatible path result");
	if (path == NULL || !IsA(path, CustomPath))
		elog(ERROR, "pg_batch expected a custom path");
	reader = pg_batch_plan_reader_create(path->custom_private,
		PATH_DATA_KIND, COMMON_DATA_VERSION);
	result->expressions = pg_batch_plan_read_list(reader, "expressions");
	result->node_data = pg_batch_plan_read_node(reader, "node_data");
	pg_batch_plan_reader_finish(reader);
}

bool
pg_batch_plan_child(const PgBatchAPI *api, const CustomPath *path,
					const List *child_plans, int child_index,
					PgBatchPlanChild *result)
{
	const PgBatchNodeOps *node;
	Path	   *child_path;
	Plan	   *child_plan;

	check_api(api);
	if (result == NULL || result->struct_size < PG_BATCH_PLAN_CHILD_MIN_SIZE)
		elog(ERROR, "pg_batch received an incompatible plan child result");
	check_plan_child_lists(path, child_plans);
	if (child_index < 0 || child_index >= list_length(child_plans))
		elog(ERROR, "pg_batch plan child index %d is out of range",
			 child_index);
	child_path = list_nth(path->custom_paths, child_index);
	child_plan = list_nth(child_plans, child_index);
	if (child_plan == NULL)
		elog(ERROR, "pg_batch received an invalid child plan");
	node = api->find_node(child_path);
	result->path = child_path;
	result->plan = child_plan;
	result->node = node;
	result->layout = (PgBatchLayout)
		PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);
	if (node == NULL)
		return false;
	node->get_layout(child_plan, &result->layout);
	pg_batch_check_layout(&result->layout);
	if (result->layout.ntargets != list_length(child_plan->targetlist))
		elog(ERROR, "pg_batch node \"%s\" returned an invalid output layout",
			 node->node_name);
	return true;
}

Plan *
pg_batch_plan_create(RelOptInfo *rel, CustomPath *path, List *targetlist,
					 List *child_plans, const PgBatchPlanConfig *config)
{
	PgBatchPlanWriter *writer;
	PgBatchLayout layout = PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);
	CustomScan *scan;
	List	   *child_names = NIL;
	List	   *target_columns = NIL;
	List	   *scan_targetlist;
	ListCell   *path_cell;
	ListCell   *plan_cell;
	bool		mapped;

	check_plan_config(config);
	check_plan_child_lists(path, child_plans);
	forboth(path_cell, path->custom_paths, plan_cell, child_plans)
	{
		Path	   *child_path = lfirst(path_cell);
		Plan	   *child_plan = lfirst(plan_cell);
		const PgBatchNodeOps *node;

		if (child_plan == NULL)
			elog(ERROR, "pg_batch received an invalid child plan");
		node = config->api->find_node(child_path);
		if (node != NULL)
			child_names = lappend(child_names,
				makeString(pstrdup(node->node_name)));
		else
			child_names = lappend(child_names, NULL);
	}

	switch (config->layout_policy)
	{
		case PG_BATCH_LAYOUT_PRESERVE_CHILD:
			{
				PgBatchPlanChild child =
					PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanChild);

				if (!pg_batch_plan_child(config->api, path, child_plans,
									 config->layout_child, &child))
					elog(ERROR, "pg_batch cannot preserve a row-only child layout");
				copy_layout(&child.layout, &layout);
				break;
			}
		case PG_BATCH_LAYOUT_DENSE:
			layout.ncolumns = list_length(targetlist);
			layout.ntargets = layout.ncolumns;
			layout.target_columns = NULL;
			break;
		case PG_BATCH_LAYOUT_EXPLICIT:
			copy_layout(config->explicit_layout, &layout);
			break;
	}
	if (layout.ntargets != list_length(targetlist))
		elog(ERROR, "pg_batch output layout does not match its target list");

	mapped = layout.target_columns != NULL;
	if (mapped)
	{
		for (int target = 0; target < layout.ntargets; target++)
			target_columns = lappend_int(target_columns,
				layout.target_columns[target]);
	}
	if (config->scan_targetlist != NIL)
		scan_targetlist = copyObject(config->scan_targetlist);
	else if (config->layout_policy == PG_BATCH_LAYOUT_PRESERVE_CHILD)
		scan_targetlist = copyObject(
			((Plan *) list_nth(child_plans,
							config->layout_child))->targetlist);
	else
		scan_targetlist = copyObject(targetlist);

	scan = makeNode(CustomScan);
	scan->methods = config->methods;
	scan->flags = path->flags;
	scan->scan.scanrelid = config->scanrelid;
	scan->scan.plan.targetlist = copyObject(targetlist);
	scan->scan.plan.qual = copyObject(config->qual);
	scan->custom_plans = child_plans;
	scan->custom_exprs = copyObject(config->expressions);
	scan->custom_scan_tlist = scan_targetlist;
	scan->custom_relids = rel == NULL ? NULL : bms_copy(rel->relids);

	writer = pg_batch_plan_writer_create(PLAN_DATA_KIND,
		COMMON_DATA_VERSION);
	pg_batch_plan_write_list(writer, "child_names", child_names);
	pg_batch_plan_write_int(writer, "layout_policy", config->layout_policy);
	pg_batch_plan_write_int(writer, "ncolumns", layout.ncolumns);
	pg_batch_plan_write_int(writer, "ntargets", layout.ntargets);
	pg_batch_plan_write_int(writer, "mapped", mapped ? 1 : 0);
	pg_batch_plan_write_int_list(writer, "target_columns", target_columns);
	pg_batch_plan_write_node(writer, "node_data", config->node_data);
	scan->custom_private = pg_batch_plan_writer_finish(writer);
	return &scan->scan.plan;
}

void
pg_batch_plan_get_info(const CustomScan *scan, PgBatchPlanInfo *result)
{
	PgBatchPlanReader *reader;
	List	   *child_names;
	List	   *target_columns;
	PgBatchLayoutPolicy layout_policy;
	bool		mapped;
	int			child = 0;
	int			target = 0;

	if (result == NULL || result->struct_size < PG_BATCH_PLAN_INFO_MIN_SIZE)
		elog(ERROR, "pg_batch received an incompatible plan result");
	if (scan == NULL || !IsA(scan, CustomScan))
		elog(ERROR, "pg_batch expected a custom plan");
	reader = pg_batch_plan_reader_create(scan->custom_private,
		PLAN_DATA_KIND, COMMON_DATA_VERSION);
	child_names = pg_batch_plan_read_list(reader, "child_names");
	result->nchildren = list_length(child_names);
	result->child_names = palloc0_array(const char *, result->nchildren);
	foreach_ptr(Node, value, child_names)
	{
		if (value != NULL && !IsA(value, String))
			elog(ERROR, "pg_batch plan received an invalid child name");
		result->child_names[child++] =
			value == NULL ? NULL : strVal(value);
	}
	result->layout = (PgBatchLayout)
		PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);
	layout_policy = pg_batch_plan_read_int(reader, "layout_policy");
	if (layout_policy < PG_BATCH_LAYOUT_PRESERVE_CHILD ||
		layout_policy > PG_BATCH_LAYOUT_EXPLICIT)
		elog(ERROR, "pg_batch plan received an invalid layout policy");
	result->layout.ncolumns = pg_batch_plan_read_int(reader, "ncolumns");
	result->layout.ntargets = pg_batch_plan_read_int(reader, "ntargets");
	mapped = pg_batch_plan_read_int(reader, "mapped") != 0;
	target_columns = pg_batch_plan_read_int_list(reader, "target_columns");
	if (mapped)
	{
		int		   *columns;

		if (list_length(target_columns) != result->layout.ntargets)
			elog(ERROR, "pg_batch plan received an invalid target mapping");
		columns = palloc_array(int, result->layout.ntargets);
		foreach_int(column, target_columns)
			columns[target++] = column;
		result->layout.target_columns = columns;
	}
	else if (target_columns != NIL)
		elog(ERROR, "pg_batch identity layout contains a target mapping");
	result->node_data = pg_batch_plan_read_node(reader, "node_data");
	pg_batch_plan_reader_finish(reader);
	if (layout_policy == PG_BATCH_LAYOUT_DENSE)
	{
		/* Core may replace a projection after PlanCustomPath returns. */
		if (result->layout.target_columns != NULL)
			pfree((void *) result->layout.target_columns);
		result->layout.ncolumns = list_length(scan->scan.plan.targetlist);
		result->layout.ntargets = result->layout.ncolumns;
		result->layout.target_columns = NULL;
	}
	pg_batch_check_layout(&result->layout);
	if (result->layout.ntargets != list_length(scan->scan.plan.targetlist))
		elog(ERROR, "pg_batch plan layout does not match its target list");
}

void
pg_batch_plan_get_layout(const Plan *plan, PgBatchLayout *result)
{
	PgBatchPlanInfo info = PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanInfo);

	if (result == NULL || result->struct_size < PG_BATCH_LAYOUT_MIN_SIZE)
		elog(ERROR, "pg_batch received an incompatible output layout");
	if (plan == NULL || !IsA(plan, CustomScan))
		elog(ERROR, "pg_batch expected a custom plan");
	pg_batch_plan_get_info((const CustomScan *) plan, &info);
	copy_layout(&info.layout, result);
	pfree(info.child_names);
	if (info.layout.target_columns != NULL)
		pfree((void *) info.layout.target_columns);
}
