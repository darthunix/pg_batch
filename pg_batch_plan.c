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
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "pg_batch.h"

static set_rel_pathlist_hook_type previous_set_rel_pathlist_hook = NULL;
static create_upper_paths_hook_type previous_create_upper_paths_hook = NULL;

typedef struct PgBatchSourceLayout
{
	List	   *targetlist;
	List	   *source_attnums;
	List	   *survivor_columns;
	int			nfilter_columns;
} PgBatchSourceLayout;

static Plan *pg_batch_plan_base(PlannerInfo *root, RelOptInfo *rel,
								CustomPath *best_path, List *tlist,
								List *clauses, List *custom_plans);
static Plan *pg_batch_plan_agg(PlannerInfo *root, RelOptInfo *rel,
							   CustomPath *best_path, List *tlist,
							   List *clauses, List *custom_plans);
static List *pg_batch_query_agg_specs(PlannerInfo *root);

static const CustomPathMethods pg_batch_base_path_methods = {
	.CustomName = "PgBatchFilterProject",
	.PlanCustomPath = pg_batch_plan_base,
};

static const CustomPathMethods pg_batch_agg_path_methods = {
	.CustomName = "PgBatchAgg",
	.PlanCustomPath = pg_batch_plan_agg,
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

static bool
pg_batch_is_scalar_operand(Node *node)
{
	return IsA(node, Const) ||
		(IsA(node, Param) &&
		 (castNode(Param, node)->paramkind == PARAM_EXTERN ||
		  castNode(Param, node)->paramkind == PARAM_EXEC));
}

static bool
pg_batch_function_is_safe(Oid funcid)
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
	if (IsA(args[0], Var) && pg_batch_is_scalar_operand(args[1]))
		*var_argno = 0;
	else if (pg_batch_is_scalar_operand(args[0]) && IsA(args[1], Var))
		*var_argno = 1;
	else
		return NULL;
	var = castNode(Var, args[*var_argno]);
	if (var->varattno <= 0 || var->varlevelsup != 0 ||
		var->varreturningtype != VAR_RETURNING_DEFAULT)
		return NULL;
	if (!pg_batch_function_is_safe(op->opfuncid))
		return NULL;
	return op;
}

static bool
pg_batch_collect_relation_attrs(Node *node, Index relid, int natts,
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

static bool
pg_batch_relation_supported(PlannerInfo *root, RelOptInfo *rel,
							RangeTblEntry *rte)
{
	Relation	relation;
	bool		result = true;

	if (!pg_batch_enable || root->parse->commandType != CMD_SELECT ||
		rel->reloptkind != RELOPT_BASEREL || rte->rtekind != RTE_RELATION ||
		rte->inh || rte->tablesample != NULL || root->parse->rowMarks != NIL ||
		rel->lateral_relids != NULL)
		return false;

	relation = table_open(rte->relid, NoLock);
	if (relation->rd_rel->relkind != RELKIND_RELATION ||
		relation->rd_rel->relam != HEAP_TABLE_AM_OID)
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

		result = pg_batch_collect_relation_attrs((Node *) quals, rel->relid,
												 RelationGetNumberOfAttributes(relation),
												 NULL);
		list_free(quals);
	}
	if (result)
		result = pg_batch_collect_relation_attrs((Node *) rel->reltarget->exprs,
												 rel->relid,
												 RelationGetNumberOfAttributes(relation),
												 NULL);
	table_close(relation, NoLock);
	return result;
}

static bool
pg_batch_base_path_is_useful(PlannerInfo *root, RelOptInfo *rel)
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
		result = pg_batch_query_agg_specs(root) != NIL;
	else
	{
		uint8		var_argno;

		result = pg_batch_match_qual(linitial(quals), &var_argno) != NULL;
	}
	list_free(quals);
	return result;
}

static void
pg_batch_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
						  RangeTblEntry *rte)
{
	CustomPath *path;

	if (previous_set_rel_pathlist_hook != NULL)
		previous_set_rel_pathlist_hook(root, rel, rti, rte);
	if (!pg_batch_relation_supported(root, rel, rte))
		return;
	if (!pg_batch_base_path_is_useful(root, rel))
		return;

	path = makeNode(CustomPath);
	path->path.pathtype = T_CustomScan;
	path->path.parent = rel;
	path->path.pathtarget = rel->reltarget;
	path->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	path->methods = &pg_batch_base_path_methods;
	cost_seqscan(&path->path, root, rel, NULL);
	/* Prefer the playground path until it has a real cost model. */
	path->path.total_cost *= 0.90;
	add_path(rel, &path->path);
}

static bool
pg_batch_parse_agg(Aggref *agg, PgBatchAggKind *kind,
				   AttrNumber *source_attnum)
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
		*source_attnum = InvalidAttrNumber;
		return true;
	}
	if (list_length(agg->args) != 1)
		return false;
	arg = linitial_node(TargetEntry, agg->args);
	expr = pg_batch_strip_relabel((Node *) arg->expr);
	if (!IsA(expr, Var) || castNode(Var, expr)->varattno <= 0 ||
		castNode(Var, expr)->varlevelsup != 0)
		return false;
	*source_attnum = castNode(Var, expr)->varattno;

	if (agg->aggfnoid == F_COUNT_ANY)
		*kind = PG_BATCH_AGG_COUNT_COLUMN;
	else if (agg->aggfnoid == F_SUM_INT4 &&
			 castNode(Var, expr)->vartype == INT4OID)
		*kind = PG_BATCH_AGG_SUM_INT4;
	else
		return false;
	return true;
}

static List *
pg_batch_build_agg_specs(List *tlist)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, tlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		PgBatchAggKind kind;
		AttrNumber	attnum;

		if (tle->resjunk || !IsA(tle->expr, Aggref) ||
			!pg_batch_parse_agg(castNode(Aggref, tle->expr), &kind, &attnum))
			return NIL;
		result = lappend(result,
						 list_make2(makeInteger(kind), makeInteger(attnum)));
	}
	return result;
}

static List *
pg_batch_query_agg_specs(PlannerInfo *root)
{
	if (!root->parse->hasAggs || root->parse->groupClause != NIL ||
		root->parse->groupingSets != NIL || root->parse->havingQual != NULL ||
		list_length(root->parse->rtable) != 1)
		return NIL;
	return pg_batch_build_agg_specs(root->processed_tlist);
}

static Path *
pg_batch_find_base_path(RelOptInfo *input_rel)
{
	Path	   *best = NULL;
	ListCell   *lc;

	foreach(lc, input_rel->pathlist)
	{
		Path	   *path = lfirst(lc);

		if (IsA(path, CustomPath) &&
			castNode(CustomPath, path)->methods == &pg_batch_base_path_methods &&
			(best == NULL || path->total_cost < best->total_cost))
			best = path;
	}
	return best;
}

static void
pg_batch_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
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
	agg_specs = pg_batch_query_agg_specs(root);
	if (agg_specs == NIL)
		return;
	child = pg_batch_find_base_path(input_rel);
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
	path->custom_private = agg_specs;
	path->methods = &pg_batch_agg_path_methods;
	add_path(output_rel, &path->path);
}

static PgBatchSourceLayout
pg_batch_build_source_layout(PlannerInfo *root, RelOptInfo *rel, List *quals,
							 List *targetlist)
{
	PgBatchSourceLayout layout = {0};
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	Relation	relation = table_open(rte->relid, NoLock);
	TupleDesc	desc = RelationGetDescr(relation);
	bool	   *filter_attrs = palloc0_array(bool, desc->natts + 1);
	bool	   *survivor_attrs = palloc0_array(bool, desc->natts + 1);
	int		   *positions = palloc0_array(int, desc->natts + 1);
	int			resno = 1;

	if (!pg_batch_collect_relation_attrs((Node *) quals, rel->relid,
										 desc->natts, filter_attrs) ||
		!pg_batch_collect_relation_attrs((Node *) targetlist, rel->relid,
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
	pfree(survivor_attrs);
	pfree(positions);
	table_close(relation, NoLock);
	return layout;
}

static CustomScan *
pg_batch_make_custom_scan(const CustomScanMethods *methods)
{
	CustomScan *scan = makeNode(CustomScan);

	scan->methods = methods;
	return scan;
}

static Plan *
pg_batch_plan_base(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
				   List *tlist, List *clauses, List *custom_plans)
{
	PgBatchSourceLayout layout;
	CustomScan *scan;
	CustomScan *filter;
	List	   *quals = extract_actual_clauses(clauses, false);
	List	   *batch_quals = NIL;
	List	   *residual;
	int			nbatch_quals = 0;

	Assert(custom_plans == NIL);
	layout = pg_batch_build_source_layout(root, rel, quals, tlist);
	foreach_ptr(Node, qual, quals)
	{
		uint8		var_argno;

		if (pg_batch_match_qual(qual, &var_argno) == NULL)
			break;
		batch_quals = lappend(batch_quals, qual);
		nbatch_quals++;
	}
	residual = list_copy_tail(quals, nbatch_quals);

	scan = pg_batch_make_custom_scan(&pg_batch_scan_plan_methods);
	scan->scan.scanrelid = rel->relid;
	scan->scan.plan.targetlist = copyObject(layout.targetlist);
	scan->scan.plan.plan_rows = rel->rows;
	scan->scan.plan.plan_width = rel->reltarget->width;
	scan->scan.plan.startup_cost = best_path->path.startup_cost;
	scan->scan.plan.total_cost = best_path->path.total_cost;
	scan->custom_exprs = copyObject(batch_quals);
	/* source attribute numbers and the compact filter-prefix length */
	scan->custom_private =
		list_make2(copyObject(layout.source_attnums),
				   makeInteger(layout.nfilter_columns));
	scan->custom_scan_tlist = copyObject(layout.targetlist);
	scan->custom_relids = bms_copy(rel->relids);

	filter = pg_batch_make_custom_scan(&pg_batch_filter_plan_methods);
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
	return &filter->scan.plan;
}

static Plan *
pg_batch_plan_agg(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
				  List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *agg;

	Assert(list_length(custom_plans) == 1);
	Assert(clauses == NIL);
	agg = pg_batch_make_custom_scan(&pg_batch_agg_plan_methods);
	agg->scan.plan.targetlist = copyObject(tlist);
	agg->custom_plans = custom_plans;
	agg->custom_private = copyObject(best_path->custom_private);
	agg->custom_scan_tlist = copyObject(tlist);
	return &agg->scan.plan;
}

void
pg_batch_planner_init(void)
{
	RegisterCustomScanMethods(&pg_batch_scan_plan_methods);
	RegisterCustomScanMethods(&pg_batch_filter_plan_methods);
	RegisterCustomScanMethods(&pg_batch_agg_plan_methods);
	previous_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = pg_batch_set_rel_pathlist;
	previous_create_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = pg_batch_create_upper_paths;
}

void
pg_batch_planner_fini(void)
{
	set_rel_pathlist_hook = previous_set_rel_pathlist_hook;
	create_upper_paths_hook = previous_create_upper_paths_hook;
}
