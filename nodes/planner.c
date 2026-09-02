#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

#include "internal.h"

static set_rel_pathlist_hook_type previous_set_rel_pathlist_hook = NULL;
static set_join_pathlist_hook_type previous_set_join_pathlist_hook = NULL;
static create_upper_paths_hook_type previous_create_upper_paths_hook = NULL;

typedef struct SourceLayout
{
	List	   *targetlist;
	List	   *source_attnums;
	List	   *project_columns;
	List	   *exact_filter_columns;
	int			nfilter_columns;
} SourceLayout;

static List *
make_base_path_data(PgBatchHeapScanMode mode)
{
	PgBatchPlanWriter *writer = pg_batch_plan_writer_create("base_path",
		PG_BATCH_PLAN_DATA_VERSION);

	pg_batch_plan_write_int(writer, "heap_scan_mode", mode);
	return pg_batch_plan_writer_finish(writer);
}

static PgBatchHeapScanMode
read_base_path_data(const CustomPath *path)
{
	PgBatchPlanReader *reader = pg_batch_plan_reader_create(
		path->custom_private, "base_path", PG_BATCH_PLAN_DATA_VERSION);
	PgBatchHeapScanMode mode = pg_batch_plan_read_int(reader,
		"heap_scan_mode");

	pg_batch_plan_reader_finish(reader);
	return mode;
}

static List *
make_hash_path_data(List *hash_quals, int planned_partitions)
{
	PgBatchPlanWriter *writer = pg_batch_plan_writer_create("hash_path",
		PG_BATCH_PLAN_DATA_VERSION);

	pg_batch_plan_write_list(writer, "hash_quals", hash_quals);
	pg_batch_plan_write_int(writer, "planned_partitions", planned_partitions);
	return pg_batch_plan_writer_finish(writer);
}

static void
read_hash_path_data(const CustomPath *path, List **hash_quals,
	int *planned_partitions)
{
	PgBatchPathInfo info = PG_BATCH_STRUCT_INITIALIZER(PgBatchPathInfo);
	PgBatchPlanReader *reader;

	pg_batch_path_get_info(path, &info);
	reader = pg_batch_plan_reader_create((List *) info.node_data,
		"hash_path", PG_BATCH_PLAN_DATA_VERSION);

	*hash_quals = pg_batch_plan_read_list(reader, "hash_quals");
	*planned_partitions = pg_batch_plan_read_int(reader,
		"planned_partitions");
	pg_batch_plan_reader_finish(reader);
}

static List *
make_agg_path_data(List *specs, bool partial)
{
	PgBatchPlanWriter *writer = pg_batch_plan_writer_create("aggregate_path",
		PG_BATCH_PLAN_DATA_VERSION);
	List	   *kinds = NIL;
	List	   *expressions = NIL;

	foreach_ptr(List, spec, specs)
	{
		kinds = lappend_int(kinds, intVal(linitial(spec)));
		expressions = lappend(expressions, copyObject(lsecond(spec)));
	}

	pg_batch_plan_write_int_list(writer, "kinds", kinds);
	pg_batch_plan_write_list(writer, "expressions", expressions);
	pg_batch_plan_write_int(writer, "partial", partial ? 1 : 0);
	return pg_batch_plan_writer_finish(writer);
}

static void
read_agg_path_data(const CustomPath *path, List **kinds,
	List **expressions, bool *partial)
{
	PgBatchPathInfo info = PG_BATCH_STRUCT_INITIALIZER(PgBatchPathInfo);
	PgBatchPlanReader *reader;

	pg_batch_path_get_info(path, &info);
	reader = pg_batch_plan_reader_create((List *) info.node_data,
		"aggregate_path", PG_BATCH_PLAN_DATA_VERSION);

	*kinds = pg_batch_plan_read_int_list(reader, "kinds");
	*expressions = pg_batch_plan_read_list(reader, "expressions");
	*partial = pg_batch_plan_read_int(reader, "partial") != 0;
	if (list_length(*kinds) != list_length(*expressions))
		elog(ERROR, "pg_batch aggregate path fields are misaligned");
	pg_batch_plan_reader_finish(reader);
}

static Plan *plan_base(PlannerInfo *root, RelOptInfo *rel,
					   CustomPath *best_path, List *tlist,
					   List *clauses, List *custom_plans);
static Plan *plan_aggregate(PlannerInfo *root, RelOptInfo *rel,
							CustomPath *best_path, List *tlist,
							List *clauses, List *custom_plans);
static List *query_aggregate_specs(PlannerInfo *root);
static Plan *plan_hash_join(PlannerInfo *root, RelOptInfo *rel,
							CustomPath *best_path, List *tlist,
							List *clauses, List *custom_plans);
static Plan *plan_pack(PlannerInfo *root, RelOptInfo *rel,
					   CustomPath *best_path, List *tlist,
					   List *clauses, List *custom_plans);
static Plan *plan_project(PlannerInfo *root, RelOptInfo *rel,
						  CustomPath *best_path, List *tlist,
						  List *clauses, List *custom_plans);

static const CustomPathMethods pg_batch_base_path_methods = {
	.CustomName = "PgBatchFilterProject",
	.PlanCustomPath = plan_base,
};

static const CustomPathMethods pg_batch_agg_path_methods = {
	.CustomName = "PgBatchAgg",
	.PlanCustomPath = plan_aggregate,
};

static const CustomPathMethods pg_batch_hash_join_path_methods = {
	.CustomName = "PgBatchHashJoin",
	.PlanCustomPath = plan_hash_join,
};

static const CustomPathMethods pg_batch_pack_path_methods = {
	.CustomName = "PgBatchPack",
	.PlanCustomPath = plan_pack,
};

static const CustomPathMethods pg_batch_project_path_methods = {
	.CustomName = "PgBatchProject",
	.PlanCustomPath = plan_project,
};

static const CustomScanMethods pg_batch_scan_plan_methods = {
	.CustomName = "PgBatchScan",
	.CreateCustomScanState = pg_batch_create_scan_state,
};

static const CustomScanMethods pg_batch_filter_plan_methods = {
	.CustomName = "PgBatchFilterProject",
	.CreateCustomScanState = pg_batch_create_filter_state,
};

static const CustomScanMethods pg_batch_agg_plan_methods = {
	.CustomName = "PgBatchAgg",
	.CreateCustomScanState = pg_batch_create_agg_state,
};

static const CustomScanMethods pg_batch_hash_join_plan_methods = {
	.CustomName = "PgBatchHashJoin",
	.CreateCustomScanState = pg_batch_create_hash_join_state,
};

static const CustomScanMethods pg_batch_pack_plan_methods = {
	.CustomName = "PgBatchPack",
	.CreateCustomScanState = pg_batch_create_pack_state,
};

static const CustomScanMethods pg_batch_project_plan_methods = {
	.CustomName = "PgBatchProject",
	.CreateCustomScanState = pg_batch_create_project_state,
};

static bool
nodes_supports_batch_path(const Path *path)
{
	const CustomPath *custom;

	if (!IsA(path, CustomPath))
		return false;
	custom = (const CustomPath *) path;
	return custom->methods == &pg_batch_base_path_methods ||
		custom->methods == &pg_batch_hash_join_path_methods ||
		custom->methods == &pg_batch_pack_path_methods ||
		custom->methods == &pg_batch_project_path_methods;
}

static int
source_attnum_column(List *source_attnums, AttrNumber attnum)
{
	int			column = 0;

	foreach_int(source_attnum, source_attnums)
	{
		if (source_attnum == attnum)
			return column;
		column++;
	}
	return -1;
}

static void
nodes_get_layout(const Plan *plan, PgBatchLayout *result)
{
	const CustomScan *scan;
	int		   *columns;
	int			column = 0;

	if (result == NULL || result->struct_size < PG_BATCH_LAYOUT_MIN_SIZE)
		elog(ERROR, "pg_batch node received an incompatible output layout");
	if (!IsA(plan, CustomScan))
		elog(ERROR, "pg_batch node expected a custom plan");
	scan = (const CustomScan *) plan;
	columns = palloc_array(int, list_length(plan->targetlist));
	if (scan->methods == &pg_batch_scan_plan_methods)
	{
		PgBatchScanPlanData data;

		pg_batch_read_scan_plan(scan, &data);
		result->ncolumns = list_length(data.source_attnums);
		while (column < list_length(plan->targetlist))
		{
			columns[column] = column;
			column++;
		}
	}
	else if (scan->methods == &pg_batch_filter_plan_methods)
	{
		PgBatchFilterPlanData data;
		List	   *source_attnums;

		pg_batch_read_filter_plan(scan, &data);
		source_attnums = data.source_attnums;

		result->ncolumns = list_length(source_attnums);

		foreach_ptr(TargetEntry, tle, plan->targetlist)
		{
			Node	   *expr = pg_batch_strip_relabel((Node *) tle->expr);

			if (!IsA(expr, Var) || castNode(Var, expr)->varattno <= 0)
				elog(ERROR, "pg_batch filter output is unavailable in batch form");
			columns[column] = source_attnum_column(source_attnums,
											 castNode(Var, expr)->varattno);
			if (columns[column] < 0)
				elog(ERROR, "pg_batch filter output column is missing from its batch");
			column++;
		}
	}
	else if (scan->methods == &pg_batch_hash_join_plan_methods ||
			 scan->methods == &pg_batch_project_plan_methods ||
			 scan->methods == &pg_batch_pack_plan_methods)
	{
		pfree(columns);
		return pg_batch_plan_get_layout(plan, result);
	}
	else
		elog(ERROR, "pg_batch node received an unknown custom plan");
	result->ntargets = column;
	result->target_columns = columns;
}

static PgBatchBinding *
nodes_get_request_binding(PlanState *planstate)
{
	TupleTableSlot *slot = pg_batch_result_slot(planstate);
	PgBatchBinding *binding = pg_batch_api->find_binding(slot);

	if (binding == NULL)
		elog(ERROR, "pg_batch node has no request binding");
	return binding;
}

const PgBatchNodeOps pg_batch_node_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_NODE_OPS_ABI_VERSION,
		PgBatchNodeOps),
	.node_name = PG_BATCH_NODE_NAME,
	.supports_path = nodes_supports_batch_path,
	.get_layout = nodes_get_layout,
	.get_request_binding = nodes_get_request_binding,
};

static bool
function_is_safe(Oid funcid)
{
	HeapTuple	tuple;
	Form_pg_proc proc;
	bool		result;

	tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);
	result = proc->prorettype == BOOLOID && !proc->proretset &&
		proc->provolatile == PROVOLATILE_IMMUTABLE && proc->proisstrict &&
		proc->proleakproof;
	ReleaseSysCache(tuple);
	return result;
}

static bool
collect_relation_attrs(Node *node, Index relid, int natts,
					   bool *attrs)
{
	List	   *vars;
	ListCell   *lc;
	bool		result = true;

	vars = pull_var_clause(node, PVC_RECURSE_AGGREGATES |
						   PVC_RECURSE_WINDOWFUNCS |
						   PVC_INCLUDE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Node	   *varnode = lfirst(lc);
		Var		   *var;

		if (!IsA(varnode, Var))
		{
			result = false;
			break;
		}
		var = castNode(Var, varnode);
		if (var->varno != relid || var->varlevelsup != 0 ||
			var->varattno <= 0 || var->varattno > natts ||
			var->varreturningtype != VAR_RETURNING_DEFAULT)
		{
			result = false;
			break;
		}
		if (attrs != NULL)
			attrs[var->varattno] = true;
	}
	list_free(vars);
	return result;
}

static Bitmapset *
relation_attnums(Node *node, Index relid)
{
	Bitmapset  *result = NULL;
	List	   *vars = pull_var_clause(node, PVC_RECURSE_AGGREGATES |
									   PVC_RECURSE_WINDOWFUNCS |
									   PVC_INCLUDE_PLACEHOLDERS);

	foreach_ptr(Node, varnode, vars)
	{
		if (IsA(varnode, Var))
		{
			Var		   *var = castNode(Var, varnode);

			if (var->varno == relid && var->varlevelsup == 0 &&
				var->varattno > 0 &&
				var->varreturningtype == VAR_RETURNING_DEFAULT)
				result = bms_add_member(result, var->varattno);
		}
	}
	list_free(vars);
	return result;
}

/* Match the bare clauses returned by extract_actual_clauses() to their rinfos. */
static List *
restriction_infos_for_clauses(List *restrictinfos, List *clauses)
{
	List	   *result = NIL;

	foreach_ptr(Node, clause, clauses)
	{
		RestrictInfo *match = NULL;

		foreach_ptr(RestrictInfo, rinfo, restrictinfos)
		{
			if ((Node *) rinfo->clause == clause)
			{
				match = rinfo;
				break;
			}
		}
		if (match == NULL)
			elog(ERROR, "pg_batch could not match a scan restriction");
		result = lappend(result, match);
	}
	return result;
}

static bool
relation_supported(PlannerInfo *root, RelOptInfo *rel,
				   RangeTblEntry *rte)
{
	Relation	relation;
	const PgBatchSourceOps *source;
	bool		result = true;

	if (!pg_batch_enable || root->parse->commandType != CMD_SELECT ||
		rel->reloptkind != RELOPT_BASEREL || rte->rtekind != RTE_RELATION ||
		rte->inh || rte->tablesample != NULL || root->parse->rowMarks != NIL ||
		rel->lateral_relids != NULL)
		return false;
	relation = table_open(rte->relid, NoLock);
	source = pg_batch_api->find_source(relation);
	if (relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		result = source != NULL && source->delivery == PG_BATCH_SOURCE_PULL;
	else if (relation->rd_rel->relkind != RELKIND_RELATION ||
			 (relation->rd_rel->relam != HEAP_TABLE_AM_OID && source == NULL))
		result = false;
	for (int i = 0; result && i < RelationGetNumberOfAttributes(relation); i++)
	{
		Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(relation), i);

		if (attr->attisdropped || attr->atthasmissing)
			result = false;
	}
	if (result)
	{
		List	   *quals = extract_actual_clauses(rel->baserestrictinfo, false);

		result = collect_relation_attrs((Node *) quals, rel->relid,
										RelationGetNumberOfAttributes(relation),
										NULL);
		list_free(quals);
	}
	if (result)
		result = collect_relation_attrs((Node *) rel->reltarget->exprs,
										rel->relid,
										RelationGetNumberOfAttributes(relation),
										NULL);
	table_close(relation, NoLock);
	return result;
}

static bool
match_int4_hash_clause(Node *node, Var **left, Var **right)
{
	OpExpr	   *op;
	Node	   *args[2];

	if (!IsA(node, OpExpr))
		return false;
	op = castNode(OpExpr, node);
	if (list_length(op->args) != 2 || op->opfuncid != F_INT4EQ)
		return false;
	args[0] = pg_batch_strip_relabel(linitial(op->args));
	args[1] = pg_batch_strip_relabel(lsecond(op->args));
	if (!IsA(args[0], Var) || !IsA(args[1], Var))
		return false;
	*left = castNode(Var, args[0]);
	*right = castNode(Var, args[1]);
	return (*left)->vartype == INT4OID && (*right)->vartype == INT4OID &&
		(*left)->varattno > 0 && (*right)->varattno > 0 &&
		(*left)->varlevelsup == 0 && (*right)->varlevelsup == 0 &&
		(*left)->varreturningtype == VAR_RETURNING_DEFAULT &&
		(*right)->varreturningtype == VAR_RETURNING_DEFAULT &&
		(*left)->varno != (*right)->varno;
}

static bool
match_int4_join_qual(Node *node)
{
	OpExpr	   *op;
	Node	   *left;
	Node	   *right;

	if (!IsA(node, OpExpr))
		return false;
	op = castNode(OpExpr, node);
	if (list_length(op->args) != 2 || !function_is_safe(op->opfuncid))
		return false;
	left = pg_batch_strip_relabel(linitial(op->args));
	right = pg_batch_strip_relabel(lsecond(op->args));
	return IsA(left, Var) && IsA(right, Var) &&
		castNode(Var, left)->vartype == INT4OID &&
		castNode(Var, right)->vartype == INT4OID;
}

static bool
base_path_is_useful(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *quals = extract_actual_clauses(rel->baserestrictinfo, false);
	bool		result;

	/*
	 * A residual-only scan makes PgBatchFilterProject adapt every tuple back
	 * to scalar ExecQual without doing any dense work. Leave that case to the
	 * ordinary executor, even when an aggregate follows. With no quals, keep
	 * the path only when a supported aggregate can consume batches directly.
	 */
	if (quals == NIL)
	{
		List	   *specs = query_aggregate_specs(root);
		int			relations = 0;

		foreach_ptr(RangeTblEntry, rte, root->parse->rtable)
		{
			if (rte->rtekind == RTE_RELATION)
				relations++;
		}
		/* A grouped query also has a synthetic RTE_GROUP entry. */
		result = relations == 1 && specs != NIL;
	}
	else
	{
		result = pg_batch_expr_supports_filter(linitial(quals), rel->relid);
	}
	list_free(quals);
	return result;
}

static bool
bitmapqual_is_brin_only(Path *path)
{
	List	   *children;

	if (IsA(path, IndexPath))
		return castNode(IndexPath, path)->indexinfo->relam == BRIN_AM_OID;
	if (IsA(path, BitmapAndPath))
		children = castNode(BitmapAndPath, path)->bitmapquals;
	else if (IsA(path, BitmapOrPath))
		children = castNode(BitmapOrPath, path)->bitmapquals;
	else
		return false;

	if (children == NIL)
		return false;
	foreach_ptr(Path, child, children)
	{
		if (!bitmapqual_is_brin_only(child))
			return false;
	}
	return true;
}

static bool
bitmap_restrictions_are_batchable(RelOptInfo *rel)
{
	foreach_ptr(RestrictInfo, rinfo, rel->baserestrictinfo)
	{
		if (!rinfo->pseudoconstant &&
			!pg_batch_expr_supports_filter((Node *) rinfo->clause,
										 rel->relid))
			return false;
	}
	return true;
}

static CustomPath *
make_base_path(RelOptInfo *rel, PgBatchHeapScanMode mode)
{
	CustomPath *path = makeNode(CustomPath);

	path->path.pathtype = T_CustomScan;
	path->path.parent = rel;
	path->path.pathtarget = rel->reltarget;
	path->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	path->custom_private = make_base_path_data(mode);
	path->methods = &pg_batch_base_path_methods;
	return path;
}

static bool
source_supports_parallel(const PgBatchSourceOps *source, Relation relation)
{
	const PgBatchSourceParallelOps *parallel;

	if (source == NULL || source->delivery != PG_BATCH_SOURCE_PULL ||
		!PG_BATCH_ABI_HAS_FIELD(source, PgBatchSourceOps, parallel) ||
		source->parallel == NULL)
		return false;
	parallel = source->parallel;
	return parallel->abi_version == PG_BATCH_SOURCE_PARALLEL_OPS_ABI_VERSION &&
		parallel->struct_size >= PG_BATCH_SOURCE_PARALLEL_OPS_MIN_SIZE &&
		parallel->supports_parallel != NULL &&
		parallel->estimate != NULL && parallel->initialize != NULL &&
		parallel->reinitialize != NULL && parallel->attach != NULL &&
		parallel->supports_parallel(relation);
}

static void
add_parallel_seq_paths(RelOptInfo *rel, Relation relation,
					   List *candidate_paths)
{
	const PgBatchSourceOps *source = pg_batch_api->find_source(relation);

	if (relation->rd_rel->relam != HEAP_TABLE_AM_OID &&
		!source_supports_parallel(source, relation))
		return;
	foreach_ptr(Path, candidate, candidate_paths)
	{
		CustomPath *path;

		if (candidate->pathtype != T_SeqScan ||
			candidate->param_info != NULL || !candidate->parallel_aware ||
			!candidate->parallel_safe)
			continue;
		path = make_base_path(rel, PG_BATCH_HEAP_SEQ);
		path->path.rows = candidate->rows;
		path->path.disabled_nodes = candidate->disabled_nodes;
		path->path.startup_cost = candidate->startup_cost;
		path->path.total_cost = candidate->total_cost * 0.90;
		path->path.pathkeys = candidate->pathkeys;
		path->path.parallel_aware = true;
		path->path.parallel_safe = true;
		path->path.parallel_workers = candidate->parallel_workers;
		add_partial_path(rel, &path->path);
	}
}

static void
add_bitmap_paths(PlannerInfo *root, RelOptInfo *rel, List *candidate_paths)
{
	List	   *bitmap_paths = NIL;
	ListCell   *lc;

	/*
	 * add_path() can free a dominated BitmapHeapPath. Keep shallow private
	 * copies before adding any alternatives; their bitmapqual trees and
	 * parent relation live for the whole planner invocation.
	 */
	foreach(lc, candidate_paths)
	{
		Path	   *candidate = lfirst(lc);
		BitmapHeapPath *bitmap;
		BitmapHeapPath *copy;
		double		rows_per_page;

		if (!IsA(candidate, BitmapHeapPath) ||
			candidate->param_info != NULL || candidate->parallel_aware)
			continue;
		bitmap = castNode(BitmapHeapPath, candidate);
		if (pg_batch_bitmap_min_rows_per_page > 0)
		{
			/* Keep automatic selection conservative around scalar residuals. */
			if (!bitmap_restrictions_are_batchable(rel))
				continue;
			if (bitmapqual_is_brin_only(bitmap->bitmapqual))
			{
				/* BRIN publishes whole lossy pages, not just matching rows. */
				rows_per_page = rel->pages > 0 ? rel->tuples / rel->pages : 0;
			}
			else
			{
				double		pages_fetched;
				double		tuples_fetched;

				pages_fetched = compute_bitmap_pages(root, rel,
												 bitmap->bitmapqual,
												 1.0, NULL, &tuples_fetched);
				rows_per_page = pages_fetched > 0 ?
					tuples_fetched / pages_fetched : 0;
			}
			if (rows_per_page < pg_batch_bitmap_min_rows_per_page)
				continue;
		}
		copy = palloc_object(BitmapHeapPath);
		memcpy(copy, bitmap, sizeof(BitmapHeapPath));
		bitmap_paths = lappend(bitmap_paths, copy);
	}

	foreach(lc, bitmap_paths)
	{
		BitmapHeapPath *bitmap = lfirst_node(BitmapHeapPath, lc);
		Path	   *candidate = &bitmap->path;
		CustomPath *path;

		path = make_base_path(rel, PG_BATCH_HEAP_BITMAP);
		path->path.rows = candidate->rows;
		path->path.disabled_nodes = candidate->disabled_nodes;
		path->path.startup_cost = candidate->startup_cost;
		path->path.total_cost = candidate->total_cost * 0.90;
		path->path.pathkeys = candidate->pathkeys;
		path->custom_paths = list_make1(bitmap);
		add_path(rel, &path->path);
	}
	list_free(bitmap_paths);
}

static void
set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
				 RangeTblEntry *rte)
{
	Relation	relation;
	List	   *candidate_paths;
	List	   *candidate_partial_paths;
	CustomPath *path;
	Path	   *foreign_path = NULL;

	if (previous_set_rel_pathlist_hook != NULL)
		previous_set_rel_pathlist_hook(root, rel, rti, rte);
	if (!relation_supported(root, rel, rte))
		return;
	if (!base_path_is_useful(root, rel))
		return;

	/*
	 * Preserve the core paths before add_path() starts comparing
	 * alternatives.
	 */
	candidate_paths = list_copy(rel->pathlist);
	candidate_partial_paths = list_copy(rel->partial_pathlist);

	/*
	 * This experiment deliberately remains heap-specific. The normal bitmap
	 * path, including its index AM and any BitmapAnd or BitmapOr tree, produces
	 * a generic TIDBitmap. PgBatchScan only changes how matching heap pages are
	 * exposed to its parent.
	 */
	relation = table_open(rte->relid, NoLock);
	if (relation->rd_rel->relam == HEAP_TABLE_AM_OID)
		add_bitmap_paths(root, rel, candidate_paths);
	add_parallel_seq_paths(rel, relation, candidate_partial_paths);
	table_close(relation, NoLock);
	list_free(candidate_partial_paths);

	path = make_base_path(rel, PG_BATCH_HEAP_SEQ);
	if (rte->relkind == RELKIND_FOREIGN_TABLE)
	{
		foreach_ptr(Path, candidate, candidate_paths)
		{
			if (!IsA(candidate, ForeignPath) || candidate->param_info != NULL ||
				candidate->parallel_aware)
				continue;
			if (foreign_path == NULL ||
				candidate->total_cost < foreign_path->total_cost)
				foreign_path = candidate;
		}
		if (foreign_path == NULL)
		{
			list_free(candidate_paths);
			return;
		}
		path->path.rows = foreign_path->rows;
		path->path.disabled_nodes = foreign_path->disabled_nodes;
		path->path.startup_cost = foreign_path->startup_cost;
		path->path.total_cost = foreign_path->total_cost;
		/* The direct source does not promise the FDW path's ordering. */
		path->path.pathkeys = NIL;
	}
	else
		cost_seqscan(&path->path, root, rel, NULL);
	list_free(candidate_paths);
	/* Prefer the playground path until it has a real cost model. */
	path->path.total_cost *= 0.90;
	add_path(rel, &path->path);
}

static bool
uses_only_int4_vars(Node *node)
{
	List	   *vars;
	bool		result = true;

	if (contain_volatile_functions(node))
		return false;
	vars = pull_var_clause(node, PVC_RECURSE_AGGREGATES |
						   PVC_RECURSE_WINDOWFUNCS |
						   PVC_INCLUDE_PLACEHOLDERS);
	foreach_ptr(Node, item, vars)
	{
		Var		   *var;

		if (!IsA(item, Var))
		{
			result = false;
			break;
		}
		var = castNode(Var, item);
		if (var->vartype != INT4OID || var->varattno <= 0 ||
			var->varlevelsup != 0 ||
			var->varreturningtype != VAR_RETURNING_DEFAULT)
		{
			result = false;
			break;
		}
	}
	list_free(vars);
	return result;
}

static bool
can_make_batch_input(PlannerInfo *root, Path *path)
{
	RelOptInfo *rel = path->parent;
	RangeTblEntry *rte;

	if (pg_batch_api->find_node(path) != NULL)
		return true;
	if (rel->reloptkind != RELOPT_BASEREL || path->param_info != NULL)
		return false;
	if (path->pathtype != T_SeqScan && !IsA(path, ForeignPath))
		return false;
	rte = planner_rt_fetch(rel->relid, root);
	return relation_supported(root, rel, rte);
}

static bool
path_outputs_supported_columns(Path *path)
{
	foreach_ptr(Node, expr, path->pathtarget->exprs)
	{
		if (exprType(expr) != INT4OID || !uses_only_int4_vars(expr))
			return false;
	}
	return true;
}

static int
path_expression_position(Path *path, Node *expr)
{
	int			position = 0;

	foreach_ptr(Node, output, path->pathtarget->exprs)
	{
		if (equal(pg_batch_strip_relabel(output),
				  pg_batch_strip_relabel(expr)))
			return position;
		position++;
	}
	return -1;
}

static bool
can_pack_input(Path *path)
{
	return path->param_info == NULL &&
		path_outputs_supported_columns(path);
}

static bool
hash_path_supported(PlannerInfo *root, HashPath *hash, RelOptInfo *outerrel,
					RelOptInfo *innerrel, bool parallel)
{
	Path	   *outer = hash->jpath.outerjoinpath;
	Path	   *inner = hash->jpath.innerjoinpath;

	if (hash->jpath.jointype != JOIN_INNER ||
		hash->jpath.path.param_info != NULL ||
		hash->jpath.path.parallel_aware != parallel ||
		outer->parent != outerrel || inner->parent != innerrel ||
		(!can_make_batch_input(root, outer) &&
		 !can_make_batch_input(root, inner)) ||
		(!can_make_batch_input(root, outer) && !can_pack_input(outer)) ||
		(!can_make_batch_input(root, inner) && !can_pack_input(inner)) ||
		!path_outputs_supported_columns(outer) ||
		!path_outputs_supported_columns(inner) ||
		hash->path_hashclauses == NIL)
		return false;

	foreach_ptr(RestrictInfo, rinfo, hash->path_hashclauses)
	{
		Var		   *left;
		Var		   *right;

		Var		   *outer_var;
		Var		   *inner_var;

		if (!OidIsValid(rinfo->hashjoinoperator) ||
			!match_int4_hash_clause((Node *) rinfo->clause, &left, &right) ||
			(!(bms_is_member(left->varno, outerrel->relids) &&
			   bms_is_member(right->varno, innerrel->relids)) &&
			 !(bms_is_member(right->varno, outerrel->relids) &&
			   bms_is_member(left->varno, innerrel->relids))))
			return false;
		if (bms_is_member(left->varno, outerrel->relids))
		{
			outer_var = left;
			inner_var = right;
		}
		else
		{
			outer_var = right;
			inner_var = left;
		}
		if (path_expression_position(outer, (Node *) outer_var) < 0 ||
			path_expression_position(inner, (Node *) inner_var) < 0)
			return false;
	}
	foreach_ptr(RestrictInfo, rinfo, hash->jpath.joinrestrictinfo)
	{
		if (!uses_only_int4_vars((Node *) rinfo->clause))
			return false;
	}
	foreach_ptr(Node, expr, hash->jpath.path.pathtarget->exprs)
	{
		if (exprType(expr) != INT4OID || !uses_only_int4_vars(expr))
			return false;
	}
	return true;
}

static Path *
make_pack_input(Path *source)
{
	PgBatchPathConfig config =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPathConfig);

	config.template_path = source;
	config.methods = &pg_batch_pack_path_methods;
	config.children = list_make1(source);
	return &pg_batch_path_create(&config)->path;
}

static Path *
make_join_input(PlannerInfo *root, Path *source)
{
	const PgBatchNodeOps *node =
		pg_batch_api->find_node(source);
	CustomPath *path;

	if (node != NULL)
		return source;
	if (!can_make_batch_input(root, source))
		return make_pack_input(source);
	Assert(source->pathtype == T_SeqScan || IsA(source, ForeignPath));
	path = make_base_path(source->parent, PG_BATCH_HEAP_SEQ);
	path->path.pathtarget = source->pathtarget;
	path->path.rows = source->rows;
	path->path.disabled_nodes = source->disabled_nodes;
	path->path.startup_cost = source->startup_cost;
	path->path.total_cost = source->total_cost;
	path->path.pathkeys = source->pathkeys;
	path->path.parallel_aware = source->parallel_aware;
	path->path.parallel_safe = source->parallel_safe;
	path->path.parallel_workers = source->parallel_workers;
	node = pg_batch_api->find_node(&path->path);
	if (node == NULL)
		elog(ERROR, "pg_batch batch input has no registered node");
	return &path->path;
}

static void
add_hash_join_path(PlannerInfo *root, RelOptInfo *joinrel, HashPath *best,
				   bool partial)
{
	CustomPath *path;
	Path	   *outer_input;
	Path	   *inner_input;
	PgBatchPathConfig config =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPathConfig);

	outer_input = make_join_input(root, best->jpath.outerjoinpath);
	inner_input = make_join_input(root, best->jpath.innerjoinpath);
	config.template_path = &best->jpath.path;
	config.methods = &pg_batch_hash_join_path_methods;
	config.children = list_make2(outer_input, inner_input);
	config.restrictinfo = best->jpath.joinrestrictinfo;
	config.node_data = (Node *) make_hash_path_data(best->path_hashclauses,
		best->num_batches);
	config.flags = CUSTOMPATH_SUPPORT_PROJECTION;
	path = pg_batch_path_create(&config);
	path->path.total_cost *= 0.85;
	if (partial)
		add_partial_path(joinrel, &path->path);
	else
		add_path(joinrel, &path->path);
}

static void
set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
				  RelOptInfo *outerrel, RelOptInfo *innerrel,
				  JoinType jointype, JoinPathExtraData *extra)
{
	HashPath   *best = NULL;
	HashPath   *best_parallel = NULL;

	if (previous_set_join_pathlist_hook != NULL)
		previous_set_join_pathlist_hook(root, joinrel, outerrel, innerrel,
								   jointype, extra);
	if (!pg_batch_enable || !pg_batch_enable_hash_join ||
		jointype != JOIN_INNER)
		return;

	foreach_ptr(Path, candidate, joinrel->pathlist)
	{
		HashPath   *hash;

		if (!IsA(candidate, HashPath))
			continue;
		hash = castNode(HashPath, candidate);
		if (hash_path_supported(root, hash, outerrel, innerrel, false) &&
			(best == NULL || candidate->total_cost < best->jpath.path.total_cost))
			best = hash;
	}
	foreach_ptr(Path, candidate, joinrel->partial_pathlist)
	{
		HashPath   *hash;

		if (!IsA(candidate, HashPath))
			continue;
		hash = castNode(HashPath, candidate);
		if (hash_path_supported(root, hash, outerrel, innerrel, true) &&
			(best_parallel == NULL ||
			 candidate->total_cost < best_parallel->jpath.path.total_cost))
			best_parallel = hash;
	}
	if (best != NULL)
		add_hash_join_path(root, joinrel, best, false);
	if (best_parallel != NULL)
		add_hash_join_path(root, joinrel, best_parallel, true);
}

static bool
parse_aggregate(Aggref *agg, PgBatchAggKind *kind, Node **source_expr)
{
	TargetEntry *arg;
	Node	   *expr;

	if (agg->aggorder != NIL || agg->aggdistinct != NIL ||
		agg->aggfilter != NULL || agg->aggdirectargs != NIL ||
		agg->agglevelsup != 0 || agg->aggsplit != AGGSPLIT_SIMPLE)
		return false;

	if (agg->aggfnoid == F_COUNT_ && agg->aggstar && agg->args == NIL)
	{
		*kind = PG_BATCH_AGG_COUNT_STAR;
		*source_expr = NULL;
		return true;
	}
	if (list_length(agg->args) != 1)
		return false;
	arg = linitial_node(TargetEntry, agg->args);
	expr = pg_batch_strip_relabel((Node *) arg->expr);
	if (!pg_batch_expr_supports_int4(expr, 0))
		return false;
	*source_expr = expr;

	if (agg->aggfnoid == F_COUNT_ANY)
		*kind = PG_BATCH_AGG_COUNT_COLUMN;
	else if (agg->aggfnoid == F_SUM_INT4)
		*kind = PG_BATCH_AGG_SUM_INT4;
	else if (agg->aggfnoid == F_MIN_INT4)
		*kind = PG_BATCH_AGG_MIN_INT4;
	else if (agg->aggfnoid == F_MAX_INT4)
		*kind = PG_BATCH_AGG_MAX_INT4;
	else
		return false;
	return true;
}

static List *
build_aggregate_specs(List *tlist)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, tlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		PgBatchAggKind kind;
		Node	   *source_expr;

		if (tle->resjunk || !IsA(tle->expr, Aggref) ||
			!parse_aggregate(castNode(Aggref, tle->expr), &kind, &source_expr))
			return NIL;
		result = lappend(result,
						 list_make2(makeInteger(kind), copyObject(source_expr)));
	}
	return result;
}

static List *
query_aggregate_specs(PlannerInfo *root)
{
	List	   *result = NIL;
	Node	   *group_expr = NULL;
	int			group_specs = 0;

	if (!root->parse->hasAggs ||
		root->parse->groupingSets != NIL || root->parse->havingQual != NULL ||
		root->parse->hasWindowFuncs)
		return NIL;
	if (root->parse->groupClause == NIL)
		return build_aggregate_specs(root->processed_tlist);
	if (list_length(root->parse->groupClause) != 1)
		return NIL;
	group_expr = pg_batch_strip_relabel((Node *)
		get_sortgroupclause_tle(linitial_node(SortGroupClause,
			root->parse->groupClause), root->processed_tlist)->expr);
	if (!IsA(group_expr, Var) || exprType(group_expr) != INT4OID)
		return NIL;

	foreach_ptr(TargetEntry, tle, root->processed_tlist)
	{
		PgBatchAggKind kind;
		Node	   *source_expr;
		Node	   *expr;

		expr = pg_batch_strip_relabel((Node *) tle->expr);
		if (equal(expr, group_expr))
		{
			result = lappend(result,
				list_make2(makeInteger(PG_BATCH_AGG_GROUP_KEY),
						   copyObject(group_expr)));
			group_specs++;
		}
		else if (IsA(expr, Aggref) &&
				 parse_aggregate(castNode(Aggref, expr), &kind, &source_expr))
			result = lappend(result,
				list_make2(makeInteger(kind), copyObject(source_expr)));
		else
			return NIL;
	}
	if (group_specs < 1)
		return NIL;
	return result;
}

static Path *
find_batch_path(List *paths)
{
	Path	   *best = NULL;
	ListCell   *lc;

	foreach(lc, paths)
	{
		Path	   *path = lfirst(lc);
		const PgBatchNodeOps *node =
			pg_batch_api->find_node(path);

		if (node != NULL &&
			(best == NULL || path->total_cost < best->total_cost))
			best = path;
	}
	return best;
}

static Path *
prepare_aggregate_child(PlannerInfo *root, RelOptInfo *input_rel,
						Path *child, List *agg_specs)
{
	bool		needs_project = false;

	foreach_ptr(List, spec, agg_specs)
	{
		Node	   *expr = lsecond(spec);

		if (expr != NULL && !IsA(pg_batch_strip_relabel(expr), Var))
		{
			needs_project = true;
			break;
		}
	}
	if (needs_project)
	{
		Path		template = {0};
		PgBatchPathConfig config =
			PG_BATCH_STRUCT_INITIALIZER(PgBatchPathConfig);
		PathTarget *target = create_empty_pathtarget();

		foreach_ptr(List, spec, agg_specs)
		{
			Node	   *expr = lsecond(spec);
			bool		found = false;

			if (expr == NULL)
				continue;
			foreach_ptr(Node, existing, target->exprs)
			{
				if (equal(pg_batch_strip_relabel(existing),
						  pg_batch_strip_relabel(expr)))
				{
					found = true;
					break;
				}
			}
			if (!found)
				add_column_to_pathtarget(target, (Expr *) copyObject(expr), 0);
		}
		set_pathtarget_cost_width(root, target);
		template.parent = input_rel;
		template.pathtarget = target;
		template.param_info = child->param_info;
		template.parallel_aware = false;
		template.parallel_safe = child->parallel_safe;
		template.parallel_workers = child->parallel_workers;
		template.rows = child->rows;
		template.startup_cost = child->startup_cost;
		template.total_cost = child->total_cost + target->cost.startup +
			target->cost.per_tuple * child->rows;
		template.disabled_nodes = child->disabled_nodes;
		config.template_path = &template;
		config.methods = &pg_batch_project_path_methods;
		config.children = list_make1(child);
		config.flags = CUSTOMPATH_SUPPORT_PROJECTION;
		child = &pg_batch_path_create(&config)->path;
	}
	return child;
}

static CustomPath *
make_batch_aggregate_path(RelOptInfo *parent, Path *child,
						  PathTarget *target, List *agg_specs,
						  bool partial, double rows)
{
	Path		template = {0};
	PgBatchPathConfig config =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPathConfig);

	template.parent = parent;
	template.pathtarget = target;
	template.param_info = child->param_info;
	template.parallel_aware = false;
	template.parallel_safe = child->parallel_safe;
	template.parallel_workers = child->parallel_workers;
	template.rows = rows;
	template.startup_cost = child->startup_cost;
	template.total_cost = child->total_cost +
		cpu_operator_cost * child->rows;
	template.disabled_nodes = child->disabled_nodes;
	config.template_path = &template;
	config.methods = &pg_batch_agg_path_methods;
	config.children = list_make1(child);
	config.node_data = (Node *) make_agg_path_data(agg_specs, partial);
	return pg_batch_path_create(&config);
}

static void
add_parallel_aggregate_path(PlannerInfo *root, RelOptInfo *input_rel,
							RelOptInfo *output_rel,
							GroupPathExtraData *extra, List *agg_specs)
{
	RelOptInfo *partial_rel;
	Path	   *child;
	CustomPath *partial;
	GatherPath *gather;
	AggPath    *final;

	/* PgBatchAgg currently emits one partial row only for global aggregates. */
	if (root->parse->groupClause != NIL || extra == NULL ||
		(extra->flags & GROUPING_CAN_PARTIAL_AGG) == 0 ||
		input_rel->partial_pathlist == NIL)
		return;
	child = find_batch_path(input_rel->partial_pathlist);
	if (child == NULL)
		return;
	partial_rel = fetch_upper_rel(root, UPPERREL_PARTIAL_GROUP_AGG,
		output_rel->relids);
	if (partial_rel->reltarget == NULL ||
		list_length(partial_rel->reltarget->exprs) != list_length(agg_specs))
		return;
	child = prepare_aggregate_child(root, input_rel, child, agg_specs);
	partial = make_batch_aggregate_path(partial_rel, child,
		partial_rel->reltarget, agg_specs, true, 1);
	add_partial_path(partial_rel, &partial->path);
	/*
	 * The upper-path hook runs after core has built its final aggregate paths.
	 * Complete this alternative here: Gather carries one transition-state row
	 * per participant and the ordinary final Agg combines those rows.
	 */
	gather = create_gather_path(root, partial_rel, &partial->path,
		partial_rel->reltarget, NULL, NULL);
	if (!extra->partial_costs_set)
	{
		get_agg_clause_costs(root, AGGSPLIT_INITIAL_SERIAL,
			&extra->agg_partial_costs);
		get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL,
			&extra->agg_final_costs);
		extra->partial_costs_set = true;
	}
	final = create_agg_path(root, output_rel, &gather->path,
		output_rel->reltarget, AGG_PLAIN, AGGSPLIT_FINAL_DESERIAL,
		NIL, NIL, &extra->agg_final_costs, 1);
	add_path(output_rel, &final->path);
}

static void
create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
				   RelOptInfo *input_rel, RelOptInfo *output_rel,
				   void *extra)
{
	Path	   *child;
	CustomPath *path;
	List	   *agg_specs;

	if (previous_create_upper_paths_hook != NULL)
		previous_create_upper_paths_hook(root, stage, input_rel, output_rel,
										 extra);
	if (!pg_batch_enable || stage != UPPERREL_GROUP_AGG)
		return;
	agg_specs = query_aggregate_specs(root);
	if (agg_specs == NIL)
		return;
	child = find_batch_path(input_rel->pathlist);
	if (child == NULL)
	{
		add_parallel_aggregate_path(root, input_rel, output_rel,
			(GroupPathExtraData *) extra, agg_specs);
		return;
	}
	child = prepare_aggregate_child(root, input_rel, child, agg_specs);
	if (root->parse->groupClause == NIL)
		path = make_batch_aggregate_path(output_rel, child,
			output_rel->reltarget, agg_specs, false, 1);
	else
	{
		List	   *group_exprs = get_sortgrouplist_exprs(
			root->processed_groupClause, root->processed_tlist);
		double		groups = estimate_num_groups(root, group_exprs,
			child->rows, NULL, NULL);

		path = make_batch_aggregate_path(output_rel, child,
			output_rel->reltarget, agg_specs, false, groups);
		list_free(group_exprs);
	}
	add_path(output_rel, &path->path);
	add_parallel_aggregate_path(root, input_rel, output_rel,
		(GroupPathExtraData *) extra, agg_specs);
}

static SourceLayout
build_source_layout(PlannerInfo *root, RelOptInfo *rel, List *local_quals,
					List *source_exprs, List *exact_quals, List *targetlist)
{
	SourceLayout layout = {0};
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	Relation	relation = table_open(rte->relid, NoLock);
	TupleDesc	desc = RelationGetDescr(relation);
	bool	   *filter_attrs = palloc0_array(bool, desc->natts + 1);
	bool	   *source_attrs = palloc0_array(bool, desc->natts + 1);
	bool	   *exact_filter_attrs = palloc0_array(bool, desc->natts + 1);
	bool	   *survivor_attrs = palloc0_array(bool, desc->natts + 1);
	int		   *positions = palloc0_array(int, desc->natts + 1);
	int			resno = 1;

	if (!collect_relation_attrs((Node *) local_quals, rel->relid,
								desc->natts, filter_attrs) ||
		!collect_relation_attrs((Node *) source_exprs, rel->relid,
								desc->natts, source_attrs) ||
		!collect_relation_attrs((Node *) exact_quals, rel->relid,
								desc->natts, exact_filter_attrs) ||
		!collect_relation_attrs((Node *) targetlist, rel->relid,
								desc->natts, survivor_attrs))
		elog(ERROR, "pg_batch found an unsupported variable");

	/*
	 * Keep the slot descriptor compact and put filter columns first. This
	 * preserves the prefix contract of getsomeattrs() without forcing it to
	 * deform unrelated table columns before a late projection column.
	 */
	for (int attnum = 1; attnum <= desc->natts; attnum++)
	{
		if (filter_attrs[attnum])
		{
			Form_pg_attribute attr = TupleDescAttr(desc, attnum - 1);
			Var		   *var = makeVar(rel->relid, attnum, attr->atttypid,
									  attr->atttypmod, attr->attcollation, 0);

			layout.targetlist = lappend(layout.targetlist,
										makeTargetEntry((Expr *) var, resno, NULL, false));
			layout.source_attnums = lappend_int(layout.source_attnums, attnum);
			positions[attnum] = resno++;
			layout.nfilter_columns++;
		}
	}
	for (int attnum = 1; attnum <= desc->natts; attnum++)
	{
		if (exact_filter_attrs[attnum])
		{
			Assert(positions[attnum] > 0);
			layout.exact_filter_columns =
				lappend_int(layout.exact_filter_columns, positions[attnum] - 1);
		}
	}
	for (int attnum = 1; attnum <= desc->natts; attnum++)
	{
		if (survivor_attrs[attnum] && !filter_attrs[attnum])
		{
			Form_pg_attribute attr = TupleDescAttr(desc, attnum - 1);
			Var		   *var = makeVar(rel->relid, attnum, attr->atttypid,
									  attr->atttypmod, attr->attcollation, 0);

			layout.targetlist = lappend(layout.targetlist,
										makeTargetEntry((Expr *) var, resno, NULL, false));
			layout.source_attnums = lappend_int(layout.source_attnums, attnum);
			positions[attnum] = resno++;
		}
	}
	/*
	 * An exact source filter may need a column that no executor expression
	 * uses. Keep such private inputs after visible columns so getsomeattrs()
	 * cannot materialize them as part of a local filter or projection prefix.
	 */
	for (int attnum = 1; attnum <= desc->natts; attnum++)
	{
		if (source_attrs[attnum] && positions[attnum] == 0)
		{
			Form_pg_attribute attr = TupleDescAttr(desc, attnum - 1);
			Var		   *var = makeVar(rel->relid, attnum, attr->atttypid,
									  attr->atttypmod, attr->attcollation, 0);

			layout.targetlist = lappend(layout.targetlist,
									makeTargetEntry((Expr *) var, resno, NULL, false));
			layout.source_attnums = lappend_int(layout.source_attnums, attnum);
			positions[attnum] = resno++;
		}
	}
	for (int attnum = 1; attnum <= desc->natts; attnum++)
	{
		if (survivor_attrs[attnum])
			layout.project_columns =
				lappend_int(layout.project_columns, positions[attnum] - 1);
	}

	pfree(filter_attrs);
	pfree(source_attrs);
	pfree(exact_filter_attrs);
	pfree(survivor_attrs);
	pfree(positions);
	table_close(relation, NoLock);
	return layout;
}

static CustomScan *
make_custom_scan(const CustomScanMethods *methods)
{
	CustomScan *scan = makeNode(CustomScan);

	scan->methods = methods;
	return scan;
}

static Plan *
plan_base(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
		  List *tlist, List *clauses, List *custom_plans)
{
	SourceLayout layout;
	PgBatchSourcePlanResult source_result =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchSourcePlanResult);
	PgBatchHeapScanMode heap_scan_mode = read_base_path_data(best_path);
	CustomScan *scan;
	CustomScan *filter;
	List	   *quals = extract_actual_clauses(clauses, false);
	List	   *batch_quals = NIL;
	List	   *batch_recheck_flags = NIL;
	List	   *residual = NIL;
	List	   *local_quals = NIL;
	List	   *exact_quals = NIL;
	List	   *bitmapqualorig = NIL;
	List	   *source_exprs = NIL;
	Node	   *source_private = (Node *) NIL;
	PgBatchPlanWriter *plan_data;
	const char *source_name = "";
	bool		dense_prefix = true;
	int			qualno = 0;

	if (heap_scan_mode == PG_BATCH_HEAP_BITMAP)
	{
		BitmapHeapScan *bitmap_scan;
		Plan	   *bitmap_plan;

		Assert(list_length(custom_plans) == 1);
		bitmap_plan = linitial(custom_plans);
		/* The outer custom path keeps the same pseudoconstant gate. */
		while (IsA(bitmap_plan, Result) && outerPlan(bitmap_plan) != NULL)
			bitmap_plan = outerPlan(bitmap_plan);
		if (!IsA(bitmap_plan, BitmapHeapScan))
			elog(ERROR, "pg_batch expected a bitmap heap child plan");
		bitmap_scan = castNode(BitmapHeapScan, bitmap_plan);
		bitmapqualorig = bitmap_scan->bitmapqualorig;
		custom_plans = list_make1(outerPlan(bitmap_scan));
	}
	else
		Assert(custom_plans == NIL);
	if (heap_scan_mode == PG_BATCH_HEAP_SEQ)
	{
		RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
		Relation	relation = table_open(rte->relid, NoLock);
		const PgBatchSourceOps *source =
			pg_batch_api->find_source(relation);

		if (source != NULL)
		{
			PgBatchSourcePlanRequest request =
				PG_BATCH_STRUCT_INITIALIZER(PgBatchSourcePlanRequest);
			List	   *restrictinfos =
				restriction_infos_for_clauses(clauses, quals);
			Bitmapset  *filter_attnums =
				relation_attnums((Node *) quals, rel->relid);
			Bitmapset  *project_attnums =
				relation_attnums((Node *) tlist, rel->relid);

			project_attnums = bms_del_members(project_attnums, filter_attnums);
			request.root = root;
			request.rel = rel;
			request.relation = relation;
			request.clauses = quals;
			request.restrictinfos = restrictinfos;
			request.filter_attnums = filter_attnums;
			request.project_attnums = project_attnums;
			source->plan_scan(&request, &source_result);
			if (source_result.nquals != list_length(quals) ||
				source_result.qual_support == NULL)
				elog(ERROR, "pg_batch source returned an invalid qualification plan");
			source_name = source->source_name;
			source_exprs = source_result.source_exprs;
			source_private = source_result.source_private;
			bms_free(filter_attnums);
			bms_free(project_attnums);
			list_free(restrictinfos);
		}
		table_close(relation, NoLock);
	}
	foreach_ptr(Node, qual, quals)
	{
		bool		recheck_only = false;

		if (source_result.qual_support != NULL &&
			source_result.qual_support[qualno] == PG_BATCH_QUAL_EXACT)
		{
			qualno++;
			continue;
		}
		local_quals = lappend(local_quals, qual);
		if (heap_scan_mode == PG_BATCH_HEAP_BITMAP &&
			list_member(bitmapqualorig, qual))
			recheck_only = true;
		if (dense_prefix &&
			pg_batch_expr_supports_filter(qual, rel->relid))
		{
			batch_quals = lappend(batch_quals, qual);
			batch_recheck_flags =
				lappend_int(batch_recheck_flags, recheck_only);
			if (!recheck_only)
				exact_quals = lappend(exact_quals, qual);
		}
		else
		{
			dense_prefix = false;
			residual = lappend(residual, qual);
			/* Residual expressions are still evaluated on exact pages. */
			exact_quals = lappend(exact_quals, qual);
		}
		qualno++;
	}
	/* Exact source filters also need their input columns in the compact batch. */
	layout = build_source_layout(root, rel, local_quals, source_exprs,
								 exact_quals, tlist);

	scan = make_custom_scan(&pg_batch_scan_plan_methods);
	scan->scan.scanrelid = rel->relid;
	scan->scan.plan.targetlist = copyObject(layout.targetlist);
	scan->scan.plan.plan_rows = rel->rows;
	scan->scan.plan.plan_width = rel->reltarget->width;
	scan->scan.plan.startup_cost = best_path->path.startup_cost;
	scan->scan.plan.total_cost = best_path->path.total_cost;
	scan->custom_exprs = list_concat(copyObject(source_exprs),
									 copyObject(batch_quals));
	scan->custom_plans = custom_plans;
	/* Compact layout, source plan, and exact-page recheck metadata. */
	plan_data = pg_batch_plan_writer_create("scan",
		PG_BATCH_PLAN_DATA_VERSION);
	pg_batch_plan_write_int_list(plan_data, "source_attnums",
		layout.source_attnums);
	pg_batch_plan_write_int(plan_data, "nfilter_columns",
		layout.nfilter_columns);
	pg_batch_plan_write_int(plan_data, "heap_scan_mode", heap_scan_mode);
	pg_batch_plan_write_string(plan_data, "source_name", source_name);
	pg_batch_plan_write_node(plan_data, "source_private", source_private);
	pg_batch_plan_write_int(plan_data, "nsource_exprs",
		list_length(source_exprs));
	pg_batch_plan_write_int_list(plan_data, "batch_recheck_flags",
		batch_recheck_flags);
	pg_batch_plan_write_int_list(plan_data, "exact_filter_columns",
		layout.exact_filter_columns);
	scan->custom_private = pg_batch_plan_writer_finish(plan_data);
	scan->custom_scan_tlist = copyObject(layout.targetlist);
	scan->custom_relids = bms_copy(rel->relids);
	scan->scan.plan.parallel_aware = best_path->path.parallel_aware;
	scan->scan.plan.parallel_safe = best_path->path.parallel_safe;

	filter = make_custom_scan(&pg_batch_filter_plan_methods);
	filter->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	filter->scan.plan.targetlist = tlist;
	filter->scan.plan.qual = residual;
	filter->custom_plans = list_make1(scan);
	/* Layout plus columns needed only for surviving rows. */
	plan_data = pg_batch_plan_writer_create("filter",
		PG_BATCH_PLAN_DATA_VERSION);
	pg_batch_plan_write_int_list(plan_data, "source_attnums",
		layout.source_attnums);
	pg_batch_plan_write_int(plan_data, "nfilter_columns",
		layout.nfilter_columns);
	pg_batch_plan_write_int_list(plan_data, "project_columns",
		layout.project_columns);
	filter->custom_private = pg_batch_plan_writer_finish(plan_data);
	filter->custom_scan_tlist = layout.targetlist;
	filter->custom_relids = bms_copy(rel->relids);
	list_free(local_quals);
	list_free(exact_quals);
	return &filter->scan.plan;
}

static Plan *
plan_pack(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
		  List *tlist, List *clauses, List *custom_plans)
{
	PgBatchPlanConfig config =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanConfig);
	Plan	   *child;

	Assert(list_length(custom_plans) == 1);
	/*
	 * Core derives clauses from the parent relation for every CustomPath.
	 * This node wraps a complete child path, which already enforces them.
	 */
	child = linitial(custom_plans);
	config.api = pg_batch_api;
	config.methods = &pg_batch_pack_plan_methods;
	config.layout_policy = PG_BATCH_LAYOUT_DENSE;
	config.scan_targetlist = child->targetlist;
	return pg_batch_plan_create(rel, best_path, tlist, custom_plans,
		&config);
}

static Plan *
plan_project(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			 List *tlist, List *clauses, List *custom_plans)
{
	PgBatchPlanChild child =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanChild);
	PgBatchPlanConfig config =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanConfig);
	PgBatchPlanWriter *plan_data;
	List	   *input_columns = NIL;
	List	   *expressions = NIL;

	Assert(list_length(custom_plans) == 1);
	/* The complete child path already enforces the clauses supplied by core. */
	if (!pg_batch_plan_child(pg_batch_api, best_path, custom_plans, 0,
							 &child))
		elog(ERROR, "pg_batch project requires a batch-aware child");
	for (int position = 0; position < child.layout.ntargets; position++)
		input_columns = lappend_int(input_columns,
			pg_batch_layout_column(&child.layout, position));
	foreach_ptr(TargetEntry, tle, tlist)
		expressions = lappend(expressions, copyObject(tle->expr));

	plan_data = pg_batch_plan_writer_create("project",
		PG_BATCH_PLAN_DATA_VERSION);
	pg_batch_plan_write_int_list(plan_data, "input_columns", input_columns);
	config.api = pg_batch_api;
	config.methods = &pg_batch_project_plan_methods;
	config.layout_policy = PG_BATCH_LAYOUT_DENSE;
	config.expressions = expressions;
	config.node_data = (Node *) pg_batch_plan_writer_finish(plan_data);
	config.scan_targetlist = child.plan->targetlist;
	return pg_batch_plan_create(rel, best_path, tlist, custom_plans,
		&config);
}

static int
target_expression_position(List *targetlist, Node *expr)
{
	int			position = 0;

	foreach_ptr(TargetEntry, tle, targetlist)
	{
		if (equal(pg_batch_strip_relabel((Node *) tle->expr),
				  pg_batch_strip_relabel(expr)))
			return position;
		position++;
	}
	return -1;
}

static List *
append_side_layout(const PgBatchPlanChild *child, List **raw_tlist)
{
	List	   *batch_columns = NIL;
	int			position = 0;

	Assert(child->node != NULL);
	foreach_ptr(TargetEntry, tle, child->plan->targetlist)
	{
		Node	   *expr = pg_batch_strip_relabel((Node *) tle->expr);
		TargetEntry *raw;

		if (exprType(expr) != INT4OID)
			elog(ERROR, "pg_batch hash join child produced a non-int4 column");
		if (pg_batch_layout_column(&child->layout, position) < 0)
			elog(ERROR, "pg_batch node \"%s\" omitted an output column",
				 child->node->node_name);
		raw = makeTargetEntry((Expr *) copyObject(tle->expr),
							  list_length(*raw_tlist) + 1,
							  NULL, false);
		*raw_tlist = lappend(*raw_tlist, raw);
		batch_columns = lappend_int(batch_columns,
			pg_batch_layout_column(&child->layout, position));
		position++;
	}
	return batch_columns;
}

static Plan *
plan_hash_join(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			   List *tlist, List *clauses, List *custom_plans)
{
	Path	   *outer_path = linitial(best_path->custom_paths);
	List	   *hash_rinfos;
	int			planned_partitions;
	PgBatchPlanChild outer =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanChild);
	PgBatchPlanChild inner =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanChild);
	PgBatchPlanConfig config =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanConfig);
	PgBatchPlanWriter *plan_data;
	List	   *all_quals;
	List	   *hash_quals;
	List	   *residual;
	List	   *batch_residual = NIL;
	List	   *scalar_residual = NIL;
	List	   *raw_tlist = NIL;
	List	   *outer_columns;
	List	   *inner_columns;
	List	   *outer_keys = NIL;
	List	   *inner_keys = NIL;
	Plan	   *outer_plan;
	Plan	   *inner_plan;

	Assert(list_length(custom_plans) == 2);
	read_hash_path_data(best_path, &hash_rinfos, &planned_partitions);
	outer_plan = linitial(custom_plans);
	inner_plan = lsecond(custom_plans);
	if (!pg_batch_plan_child(pg_batch_api, best_path, custom_plans, 0,
							 &outer) ||
		!pg_batch_plan_child(pg_batch_api, best_path, custom_plans, 1,
							 &inner))
		elog(ERROR, "pg_batch hash join requires batch-aware children");
	outer_columns = append_side_layout(&outer, &raw_tlist);
	inner_columns = append_side_layout(&inner, &raw_tlist);

	foreach_ptr(RestrictInfo, rinfo, hash_rinfos)
	{
		OpExpr	   *op = castNode(OpExpr, rinfo->clause);
		Var		   *left = castNode(Var,
								 pg_batch_strip_relabel(linitial(op->args)));
		Var		   *right = castNode(Var,
								  pg_batch_strip_relabel(lsecond(op->args)));
		Var		   *outer_var;
		Var		   *inner_var;
		int			outer_column;
		int			inner_column;

		if (bms_is_member(left->varno, outer_path->parent->relids))
		{
			outer_var = left;
			inner_var = right;
		}
		else
		{
			outer_var = right;
			inner_var = left;
		}
		outer_column = target_expression_position(outer_plan->targetlist,
										  (Node *) outer_var);
		inner_column = target_expression_position(inner_plan->targetlist,
										  (Node *) inner_var);
		if (outer_column < 0 || inner_column < 0)
			elog(ERROR, "pg_batch hash key is missing from a child target");
		outer_keys = lappend_int(outer_keys, outer_column);
		inner_keys = lappend_int(inner_keys, inner_column);
	}

	all_quals = get_actual_clauses(best_path->custom_restrictinfo);
	hash_quals = get_actual_clauses(hash_rinfos);
	residual = list_difference(all_quals, hash_quals);
	foreach_ptr(Node, qual, residual)
	{
		if (match_int4_join_qual(qual))
			batch_residual = lappend(batch_residual, qual);
		else
			scalar_residual = lappend(scalar_residual, qual);
	}

	plan_data = pg_batch_plan_writer_create("hash_join",
		PG_BATCH_PLAN_DATA_VERSION);
	pg_batch_plan_write_int_list(plan_data, "outer_columns", outer_columns);
	pg_batch_plan_write_int_list(plan_data, "inner_columns", inner_columns);
	pg_batch_plan_write_int_list(plan_data, "outer_keys", outer_keys);
	pg_batch_plan_write_int_list(plan_data, "inner_keys", inner_keys);
	pg_batch_plan_write_int(plan_data, "planned_partitions",
		planned_partitions);
	config.api = pg_batch_api;
	config.methods = &pg_batch_hash_join_plan_methods;
	config.layout_policy = PG_BATCH_LAYOUT_DENSE;
	config.qual = scalar_residual;
	config.expressions = batch_residual;
	config.node_data = (Node *) pg_batch_plan_writer_finish(plan_data);
	config.scan_targetlist = raw_tlist;
	return pg_batch_plan_create(rel, best_path, tlist, custom_plans,
		&config);
}

static Plan *
plan_aggregate(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			   List *tlist, List *clauses, List *custom_plans)
{
	List	   *agg_kinds;
	List	   *agg_exprs;
	ListCell   *kind_cell;
	ListCell   *expr_cell;
	PgBatchPlanChild child =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanChild);
	PgBatchPlanConfig config =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanConfig);
	PgBatchPlanWriter *plan_data;
	List	   *runtime_columns = NIL;
	bool		partial;

	Assert(list_length(custom_plans) == 1);
	Assert(clauses == NIL);
	read_agg_path_data(best_path, &agg_kinds, &agg_exprs, &partial);
	if (!pg_batch_plan_child(pg_batch_api, best_path, custom_plans, 0,
							 &child))
		elog(ERROR, "pg_batch aggregate requires a batch-aware child");
	forboth(kind_cell, agg_kinds, expr_cell, agg_exprs)
	{
		Node	   *expr = lfirst(expr_cell);
		int			column = -1;

		if (expr != NULL)
		{
			int			position = target_expression_position(child.plan->targetlist,
												 expr);

			if (position >= 0)
				column = pg_batch_layout_column(&child.layout, position);
			if (column < 0)
				elog(ERROR, "pg_batch aggregate input is missing from child target");
		}
		runtime_columns = lappend_int(runtime_columns, column);
	}
	plan_data = pg_batch_plan_writer_create("aggregate",
		PG_BATCH_PLAN_DATA_VERSION);
	pg_batch_plan_write_int_list(plan_data, "kinds", agg_kinds);
	pg_batch_plan_write_int_list(plan_data, "columns", runtime_columns);
	pg_batch_plan_write_int(plan_data, "partial", partial ? 1 : 0);
	config.api = pg_batch_api;
	config.methods = &pg_batch_agg_plan_methods;
	config.layout_policy = PG_BATCH_LAYOUT_DENSE;
	config.node_data = (Node *) pg_batch_plan_writer_finish(plan_data);
	return pg_batch_plan_create(rel, best_path, tlist, custom_plans,
		&config);
}

void
pg_batch_planner_init(void)
{
	RegisterCustomScanMethods(&pg_batch_scan_plan_methods);
	RegisterCustomScanMethods(&pg_batch_filter_plan_methods);
	RegisterCustomScanMethods(&pg_batch_pack_plan_methods);
	RegisterCustomScanMethods(&pg_batch_project_plan_methods);
	RegisterCustomScanMethods(&pg_batch_hash_join_plan_methods);
	RegisterCustomScanMethods(&pg_batch_agg_plan_methods);
	previous_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = set_rel_pathlist;
	previous_set_join_pathlist_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = set_join_pathlist;
	previous_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = create_upper_paths;
}

void
pg_batch_planner_fini(void)
{
	set_rel_pathlist_hook = previous_set_rel_pathlist_hook;
	set_join_pathlist_hook = previous_set_join_pathlist_hook;
	create_upper_paths_hook = previous_create_upper_paths_hook;
}
