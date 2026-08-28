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
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "internal.h"

static set_rel_pathlist_hook_type previous_set_rel_pathlist_hook = NULL;
static set_join_pathlist_hook_type previous_set_join_pathlist_hook = NULL;
static create_upper_paths_hook_type previous_create_upper_paths_hook = NULL;

typedef struct SourceLayout
{
	List	   *targetlist;
	List	   *source_attnums;
	List	   *survivor_columns;
	List	   *exact_filter_columns;
	int			nfilter_columns;
} SourceLayout;

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

static bool
nodes_supports_batch_path(const Path *path)
{
	const CustomPath *custom;

	if (!IsA(path, CustomPath))
		return false;
	custom = castNode(CustomPath, path);
	return custom->methods == &pg_batch_base_path_methods ||
		custom->methods == &pg_batch_hash_join_path_methods ||
		custom->methods == &pg_batch_pack_path_methods;
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
nodes_get_output_layout(const Plan *plan,
						PgBatchBridgeOutputLayout *result)
{
	const CustomScan *scan;
	int		   *columns;
	int			column = 0;

	if (!IsA(plan, CustomScan))
		elog(ERROR, "pg_batch producer expected a custom plan");
	scan = castNode(CustomScan, plan);
	columns = palloc_array(int, list_length(plan->targetlist));
	if (scan->methods == &pg_batch_filter_plan_methods)
	{
		List	   *source_attnums = linitial(scan->custom_private);

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
			 scan->methods == &pg_batch_pack_plan_methods)
	{
		while (column < list_length(plan->targetlist))
		{
			columns[column] = column;
			column++;
		}
	}
	else
		elog(ERROR, "pg_batch producer received an unknown custom plan");
	result->ncolumns = column;
	result->batch_columns = columns;
}

static PgBatchBridgeBinding *
nodes_get_request_binding(PlanState *planstate)
{
	TupleTableSlot *slot = pg_batch_result_batch_slot(planstate);

	return pg_batch_slot_cast(slot)->binding;
}

const PgBatchBridgeProducerOps pg_batch_producer_ops = {
	.abi_version = PG_BATCH_BRIDGE_ABI_VERSION,
	.struct_size = sizeof(PgBatchBridgeProducerOps),
	.producer_name = PG_BATCH_PRODUCER_NAME,
	.supports_path = nodes_supports_batch_path,
	.get_output_layout = nodes_get_output_layout,
	.get_request_binding = nodes_get_request_binding,
};

static bool
is_scalar_operand(Node *node)
{
	return IsA(node, Const) ||
		(IsA(node, Param) &&
		 (castNode(Param, node)->paramkind == PARAM_EXTERN ||
		  castNode(Param, node)->paramkind == PARAM_EXEC));
}

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

OpExpr *
pg_batch_match_qual(Node *clause, uint8 *var_argno)
{
	OpExpr	   *op;
	Node	   *args[2];
	Var		   *var;

	if (!IsA(clause, OpExpr))
		return NULL;
	op = castNode(OpExpr, clause);
	if (list_length(op->args) != 2)
		return NULL;
	args[0] = pg_batch_strip_relabel(linitial(op->args));
	args[1] = pg_batch_strip_relabel(lsecond(op->args));
	if (IsA(args[0], Var) && is_scalar_operand(args[1]))
		*var_argno = 0;
	else if (is_scalar_operand(args[0]) && IsA(args[1], Var))
		*var_argno = 1;
	else
		return NULL;
	var = castNode(Var, args[*var_argno]);
	if (var->varattno <= 0 || var->varlevelsup != 0 ||
		var->varreturningtype != VAR_RETURNING_DEFAULT)
		return NULL;
	if (!function_is_safe(op->opfuncid))
		return NULL;
	return op;
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
	const PgBatchBridgeProviderOps *provider;
	bool		result = true;

	if (!pg_batch_enable || root->parse->commandType != CMD_SELECT ||
		rel->reloptkind != RELOPT_BASEREL || rte->rtekind != RTE_RELATION ||
		rte->inh || rte->tablesample != NULL || root->parse->rowMarks != NIL ||
		rel->lateral_relids != NULL)
		return false;
	relation = table_open(rte->relid, NoLock);
	provider = pg_batch_bridge->find_provider(relation);
	if (relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		result = provider != NULL && provider->next_batch != NULL;
	else if (relation->rd_rel->relkind != RELKIND_RELATION ||
			 (relation->rd_rel->relam != HEAP_TABLE_AM_OID && provider == NULL))
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

		result = list_length(root->parse->rtable) == 1 && specs != NIL;
		foreach_ptr(List, spec, specs)
		{
			Node	   *expr = lsecond(spec);

			if (expr != NULL && !IsA(pg_batch_strip_relabel(expr), Var))
			{
				result = false;
				break;
			}
		}
	}
	else
	{
		uint8		var_argno;

		result = pg_batch_match_qual(linitial(quals), &var_argno) != NULL;
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
		uint8		var_argno;

		if (!rinfo->pseudoconstant &&
			pg_batch_match_qual((Node *) rinfo->clause, &var_argno) == NULL)
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
	path->custom_private = list_make1(makeInteger(mode));
	path->methods = &pg_batch_base_path_methods;
	return path;
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

	/*
	 * This experiment deliberately remains heap-specific. The normal bitmap
	 * path, including its index AM and any BitmapAnd or BitmapOr tree, produces
	 * a generic TIDBitmap. PgBatchScan only changes how matching heap pages are
	 * exposed to its parent.
	 */
	relation = table_open(rte->relid, NoLock);
	if (relation->rd_rel->relam == HEAP_TABLE_AM_OID)
		add_bitmap_paths(root, rel, candidate_paths);
	table_close(relation, NoLock);

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
		/* The direct provider does not promise the FDW path's ordering. */
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

	if (pg_batch_bridge->find_producer(path) != NULL)
		return true;
	if (rel->reloptkind != RELOPT_BASEREL || path->param_info != NULL ||
		path->parallel_aware)
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
	return path->param_info == NULL && !path->parallel_aware &&
		path_outputs_supported_columns(path);
}

static bool
hash_path_supported(PlannerInfo *root, HashPath *hash, RelOptInfo *outerrel,
					RelOptInfo *innerrel)
{
	Path	   *outer = hash->jpath.outerjoinpath;
	Path	   *inner = hash->jpath.innerjoinpath;

	if (hash->jpath.jointype != JOIN_INNER ||
		hash->jpath.path.param_info != NULL ||
		hash->jpath.path.parallel_aware ||
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
	CustomPath *path;

	path = makeNode(CustomPath);
	path->path = *source;
	NodeSetTag(path, T_CustomPath);
	path->path.pathtype = T_CustomScan;
	path->path.parallel_aware = false;
	path->flags = 0;
	path->custom_paths = list_make1(source);
	path->methods = &pg_batch_pack_path_methods;
	return &path->path;
}

static Path *
make_join_input(PlannerInfo *root, Path *source, const char **producer_name)
{
	const PgBatchBridgeProducerOps *producer =
		pg_batch_bridge->find_producer(source);
	CustomPath *path;

	if (producer != NULL)
	{
		*producer_name = producer->producer_name;
		return source;
	}
	if (!can_make_batch_input(root, source))
	{
		*producer_name = PG_BATCH_PRODUCER_NAME;
		return make_pack_input(source);
	}
	Assert(source->pathtype == T_SeqScan || IsA(source, ForeignPath));
	path = make_base_path(source->parent, PG_BATCH_HEAP_SEQ);
	path->path.pathtarget = source->pathtarget;
	path->path.rows = source->rows;
	path->path.disabled_nodes = source->disabled_nodes;
	path->path.startup_cost = source->startup_cost;
	path->path.total_cost = source->total_cost;
	path->path.pathkeys = source->pathkeys;
	producer = pg_batch_bridge->find_producer(&path->path);
	if (producer == NULL)
		elog(ERROR, "pg_batch batch input has no registered producer");
	*producer_name = producer->producer_name;
	return &path->path;
}

static void
set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
				  RelOptInfo *outerrel, RelOptInfo *innerrel,
				  JoinType jointype, JoinPathExtraData *extra)
{
	HashPath   *best = NULL;
	CustomPath *path;
	Path	   *outer_input;
	Path	   *inner_input;
	const char *outer_producer;
	const char *inner_producer;

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
		if (hash_path_supported(root, hash, outerrel, innerrel) &&
			(best == NULL || candidate->total_cost < best->jpath.path.total_cost))
			best = hash;
	}
	if (best == NULL)
		return;

	path = makeNode(CustomPath);
	path->path = best->jpath.path;
	NodeSetTag(path, T_CustomPath);
	path->path.pathtype = T_CustomScan;
	path->path.parallel_aware = false;
	path->path.parallel_safe = false;
	path->path.total_cost *= 0.85;
	path->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	outer_input = make_join_input(root, best->jpath.outerjoinpath,
								  &outer_producer);
	inner_input = make_join_input(root, best->jpath.innerjoinpath,
								  &inner_producer);
	path->custom_paths = list_make2(outer_input, inner_input);
	path->custom_restrictinfo = copyObject(best->jpath.joinrestrictinfo);
	path->custom_private =
		list_make4(copyObject(best->path_hashclauses),
				   makeInteger(best->num_batches),
				   makeString(pstrdup(outer_producer)),
				   makeString(pstrdup(inner_producer)));
	path->methods = &pg_batch_hash_join_path_methods;
	add_path(joinrel, &path->path);
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
	if (!IsA(expr, Var) || castNode(Var, expr)->vartype != INT4OID ||
		castNode(Var, expr)->varattno <= 0 ||
		castNode(Var, expr)->varlevelsup != 0 ||
		castNode(Var, expr)->varreturningtype != VAR_RETURNING_DEFAULT)
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
	if (!root->parse->hasAggs || root->parse->groupClause != NIL ||
		root->parse->groupingSets != NIL || root->parse->havingQual != NULL ||
		root->parse->hasWindowFuncs)
		return NIL;
	return build_aggregate_specs(root->processed_tlist);
}

static Path *
find_batch_path(RelOptInfo *input_rel, const char **producer_name)
{
	Path	   *best = NULL;
	const PgBatchBridgeProducerOps *best_producer = NULL;
	ListCell   *lc;

	foreach(lc, input_rel->pathlist)
	{
		Path	   *path = lfirst(lc);
		const PgBatchBridgeProducerOps *producer =
			pg_batch_bridge->find_producer(path);

		if (producer != NULL &&
			(best == NULL || path->total_cost < best->total_cost))
		{
			best = path;
			best_producer = producer;
		}
	}
	*producer_name = best_producer == NULL ? NULL : best_producer->producer_name;
	return best;
}

static void
create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
				   RelOptInfo *input_rel, RelOptInfo *output_rel,
				   void *extra)
{
	Path	   *child;
	CustomPath *path;
	List	   *agg_specs;
	const char *producer_name;

	if (previous_create_upper_paths_hook != NULL)
		previous_create_upper_paths_hook(root, stage, input_rel, output_rel,
										 extra);
	if (!pg_batch_enable || stage != UPPERREL_GROUP_AGG)
		return;
	agg_specs = query_aggregate_specs(root);
	if (agg_specs == NIL)
		return;
	child = find_batch_path(input_rel, &producer_name);
	if (child == NULL)
		return;

	path = makeNode(CustomPath);
	path->path.pathtype = T_CustomScan;
	path->path.parent = output_rel;
	path->path.pathtarget = output_rel->reltarget;
	path->path.rows = 1;
	path->path.startup_cost = child->startup_cost;
	path->path.total_cost = child->total_cost + cpu_operator_cost * child->rows;
	path->path.disabled_nodes = child->disabled_nodes;
	path->custom_paths = list_make1(child);
	path->custom_private =
		list_make2(makeString(pstrdup(producer_name)), agg_specs);
	path->methods = &pg_batch_agg_path_methods;
	add_path(output_rel, &path->path);
}

static SourceLayout
build_source_layout(PlannerInfo *root, RelOptInfo *rel, List *quals,
					List *exact_quals, List *targetlist)
{
	SourceLayout layout = {0};
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	Relation	relation = table_open(rte->relid, NoLock);
	TupleDesc	desc = RelationGetDescr(relation);
	bool	   *filter_attrs = palloc0_array(bool, desc->natts + 1);
	bool	   *exact_filter_attrs = palloc0_array(bool, desc->natts + 1);
	bool	   *survivor_attrs = palloc0_array(bool, desc->natts + 1);
	int		   *positions = palloc0_array(int, desc->natts + 1);
	int			resno = 1;

	if (!collect_relation_attrs((Node *) quals, rel->relid,
								desc->natts, filter_attrs) ||
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
	for (int attnum = 1; attnum <= desc->natts; attnum++)
	{
		if (survivor_attrs[attnum])
			layout.survivor_columns =
				lappend_int(layout.survivor_columns, positions[attnum] - 1);
	}

	pfree(filter_attrs);
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
	PgBatchBridgePlanResult source_result;
	PgBatchHeapScanMode heap_scan_mode =
		intVal(linitial(best_path->custom_private));
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
	const char *provider_name = "";
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
	MemSet(&source_result, 0, sizeof(source_result));
	if (heap_scan_mode == PG_BATCH_HEAP_SEQ)
	{
		RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
		Relation	relation = table_open(rte->relid, NoLock);
		const PgBatchBridgeProviderOps *provider =
			pg_batch_bridge->find_provider(relation);

		if (provider != NULL)
		{
			PgBatchBridgePlanRequest request;
			List	   *restrictinfos =
				restriction_infos_for_clauses(clauses, quals);
			Bitmapset  *filter_attnums =
				relation_attnums((Node *) quals, rel->relid);
			Bitmapset  *project_attnums =
				relation_attnums((Node *) tlist, rel->relid);

			project_attnums = bms_del_members(project_attnums, filter_attnums);
			MemSet(&request, 0, sizeof(request));
			request.root = root;
			request.rel = rel;
			request.relation = relation;
			request.clauses = quals;
			request.restrictinfos = restrictinfos;
			request.filter_attnums = filter_attnums;
			request.project_attnums = project_attnums;
			provider->plan_scan(&request, &source_result);
			if (source_result.nquals != list_length(quals) ||
				source_result.qual_support == NULL)
				elog(ERROR, "pg_batch provider returned an invalid qualification plan");
			provider_name = provider->provider_name;
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
		uint8		var_argno;
		bool		recheck_only = false;

		if (source_result.qual_support != NULL &&
			source_result.qual_support[qualno] == PG_BATCH_BRIDGE_QUAL_EXACT)
		{
			qualno++;
			continue;
		}
		local_quals = lappend(local_quals, qual);
		if (heap_scan_mode == PG_BATCH_HEAP_BITMAP &&
			list_member(bitmapqualorig, qual))
			recheck_only = true;
		if (dense_prefix && pg_batch_match_qual(qual, &var_argno) != NULL)
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
	layout = build_source_layout(root, rel, local_quals, exact_quals, tlist);

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
	scan->custom_private =
		list_make5(copyObject(layout.source_attnums),
				   makeInteger(layout.nfilter_columns),
				   makeInteger(heap_scan_mode),
				   makeString(pstrdup(provider_name)),
				   copyObject(source_private));
	scan->custom_private = lappend(scan->custom_private,
								   makeInteger(list_length(source_exprs)));
	scan->custom_private = lappend(scan->custom_private,
								   batch_recheck_flags);
	scan->custom_private = lappend(scan->custom_private,
								   layout.exact_filter_columns);
	scan->custom_scan_tlist = copyObject(layout.targetlist);
	scan->custom_relids = bms_copy(rel->relids);

	filter = make_custom_scan(&pg_batch_filter_plan_methods);
	filter->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	filter->scan.plan.targetlist = tlist;
	filter->scan.plan.qual = residual;
	filter->custom_plans = list_make1(scan);
	/* layout plus columns needed only for surviving rows */
	filter->custom_private =
		list_make3(layout.source_attnums,
				   makeInteger(layout.nfilter_columns),
				   layout.survivor_columns);
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
	CustomScan *pack;
	Plan	   *child;

	Assert(list_length(custom_plans) == 1);
	Assert(clauses == NIL);
	child = linitial(custom_plans);
	pack = make_custom_scan(&pg_batch_pack_plan_methods);
	pack->scan.plan.targetlist = tlist;
	pack->custom_plans = custom_plans;
	pack->custom_scan_tlist = copyObject(child->targetlist);
	pack->custom_relids = bms_copy(rel->relids);
	return &pack->scan.plan;
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
append_side_layout(Plan *child, const char *producer_name, List **raw_tlist)
{
	const PgBatchBridgeProducerOps *producer =
		pg_batch_bridge->get_producer(producer_name);
	PgBatchBridgeOutputLayout layout;
	List	   *batch_columns = NIL;
	int			position = 0;

	if (producer == NULL)
		elog(ERROR, "pg_batch producer \"%s\" is not registered",
			 producer_name);
	MemSet(&layout, 0, sizeof(layout));
	producer->get_output_layout(child, &layout);
	if (layout.ncolumns != list_length(child->targetlist) ||
		(layout.ncolumns > 0 && layout.batch_columns == NULL))
		elog(ERROR, "pg_batch producer \"%s\" returned an invalid output layout",
			 producer_name);
	foreach_ptr(TargetEntry, tle, child->targetlist)
	{
		Node	   *expr = pg_batch_strip_relabel((Node *) tle->expr);
		TargetEntry *raw;

		if (exprType(expr) != INT4OID)
			elog(ERROR, "pg_batch hash join child produced a non-int4 column");
		if (layout.batch_columns[position] < 0)
			elog(ERROR, "pg_batch producer \"%s\" omitted an output column",
				 producer_name);
		raw = makeTargetEntry((Expr *) copyObject(tle->expr),
							  list_length(*raw_tlist) + 1,
							  NULL, false);
		*raw_tlist = lappend(*raw_tlist, raw);
		batch_columns = lappend_int(batch_columns,
									layout.batch_columns[position]);
		position++;
	}
	return batch_columns;
}

static Plan *
plan_hash_join(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			   List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *join;
	Path	   *outer_path = linitial(best_path->custom_paths);
	List	   *hash_rinfos = linitial(best_path->custom_private);
	const char *outer_producer = strVal(lthird(best_path->custom_private));
	const char *inner_producer = strVal(lfourth(best_path->custom_private));
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
	Assert(list_length(best_path->custom_private) == 4);
	outer_plan = linitial(custom_plans);
	inner_plan = lsecond(custom_plans);
	outer_columns = append_side_layout(outer_plan, outer_producer, &raw_tlist);
	inner_columns = append_side_layout(inner_plan, inner_producer, &raw_tlist);

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

	join = make_custom_scan(&pg_batch_hash_join_plan_methods);
	join->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	join->scan.plan.targetlist = tlist;
	join->scan.plan.qual = scalar_residual;
	join->custom_plans = custom_plans;
	join->custom_exprs = batch_residual;
	join->custom_private =
		list_make5(outer_columns, inner_columns, outer_keys, inner_keys,
				   copyObject(lsecond(best_path->custom_private)));
	join->custom_private = lappend(join->custom_private,
								  makeString(pstrdup(outer_producer)));
	join->custom_private = lappend(join->custom_private,
								  makeString(pstrdup(inner_producer)));
	join->custom_scan_tlist = raw_tlist;
	join->custom_relids = bms_copy(rel->relids);
	return &join->scan.plan;
}

static Plan *
plan_aggregate(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			   List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *agg;
	Plan	   *child;
	const char *producer_name = strVal(linitial(best_path->custom_private));
	List	   *agg_specs = lsecond_node(List, best_path->custom_private);
	const PgBatchBridgeProducerOps *producer;
	PgBatchBridgeOutputLayout layout;
	List	   *runtime_specs = NIL;

	Assert(list_length(custom_plans) == 1);
	Assert(list_length(best_path->custom_private) == 2);
	Assert(clauses == NIL);
	child = linitial(custom_plans);
	producer = pg_batch_bridge->get_producer(producer_name);
	if (producer == NULL)
		elog(ERROR, "pg_batch producer \"%s\" is not registered",
			 producer_name);
	MemSet(&layout, 0, sizeof(layout));
	producer->get_output_layout(child, &layout);
	if (layout.ncolumns != list_length(child->targetlist) ||
		(layout.ncolumns > 0 && layout.batch_columns == NULL))
		elog(ERROR, "pg_batch producer \"%s\" returned an invalid output layout",
			 producer_name);
	foreach_ptr(List, spec, agg_specs)
	{
		PgBatchAggKind kind = intVal(linitial(spec));
		Node	   *expr = lsecond(spec);
		int			column = -1;

		if (expr != NULL)
		{
			int			position = target_expression_position(child->targetlist,
													 expr);

			if (position >= 0)
				column = layout.batch_columns[position];
			if (column < 0)
				elog(ERROR, "pg_batch aggregate input is missing from child target");
		}
		runtime_specs = lappend(runtime_specs,
								 list_make2(makeInteger(kind),
											makeInteger(column)));
	}
	agg = make_custom_scan(&pg_batch_agg_plan_methods);
	agg->scan.plan.targetlist = copyObject(tlist);
	agg->custom_plans = custom_plans;
	agg->custom_private =
		list_make2(makeString(pstrdup(producer_name)), runtime_specs);
	agg->custom_scan_tlist = copyObject(tlist);
	return &agg->scan.plan;
}

void
pg_batch_planner_init(void)
{
	RegisterCustomScanMethods(&pg_batch_scan_plan_methods);
	RegisterCustomScanMethods(&pg_batch_filter_plan_methods);
	RegisterCustomScanMethods(&pg_batch_pack_plan_methods);
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
