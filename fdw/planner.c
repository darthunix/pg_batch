#include "postgres.h"

#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type_d.h"
#include "foreign/foreign.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/optimizer.h"
#include "optimizer/restrictinfo.h"

#include "internal.h"

static void
classify_quals(RelOptInfo *rel, List *clauses, List *restrictinfos,
			   PgBatchBridgeQualSupport *support,
			   List **source_exprs, List **specs)
{
	ListCell   *clause_cell;
	ListCell   *rinfo_cell;
	int			qualno = 0;
	int			exprno = 0;

	if (list_length(clauses) != list_length(restrictinfos))
		elog(ERROR, "pg_batch_fdw received misaligned restrictions");
	forboth(clause_cell, clauses, rinfo_cell, restrictinfos)
	{
		Node	   *clause = lfirst(clause_cell);
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, rinfo_cell);
		if (pg_batch_fdw_pushdown &&
			restriction_is_securely_promotable(rinfo, rel) &&
			pg_batch_expr_supports_filter(clause, rel->relid))
		{
			support[qualno] = PG_BATCH_BRIDGE_QUAL_EXACT;
			*source_exprs = lappend(*source_exprs, copyObject(clause));
			*specs = lappend_int(*specs, exprno++);
		}
		qualno++;
	}
}

static bool
supports_relation(Relation relation)
{
	ForeignTable *table;
	ForeignServer *server;
	ForeignDataWrapper *wrapper;

	if (relation->rd_rel->relkind != RELKIND_FOREIGN_TABLE)
		return false;
	table = GetForeignTable(RelationGetRelid(relation));
	server = GetForeignServer(table->serverid);
	wrapper = GetForeignDataWrapper(server->fdwid);
	return strcmp(wrapper->fdwname, "pg_batch_fdw") == 0;
}

static void
provider_plan_scan(const PgBatchBridgePlanRequest *request,
				   PgBatchBridgePlanResult *result)
{
	List	   *source_exprs = NIL;
	List	   *specs = NIL;
	int			nquals = list_length(request->clauses);

	MemSet(result, 0, sizeof(*result));
	result->nquals = nquals;
	result->qual_support = palloc0_array(PgBatchBridgeQualSupport, nquals);
	classify_quals(request->rel, request->clauses, request->restrictinfos,
				   result->qual_support, &source_exprs, &specs);
	result->source_exprs = source_exprs;
	result->source_private = (Node *) specs;
}

static void *
provider_begin_scan(const PgBatchBridgeExecRequest *request)
{
	return pg_batch_fdw_scan_begin(request->relation, request->parent,
								   request->source_private,
								   request->source_exprs,
								   request->slot_request,
								   request->query_context);
}

static PgBatchBridgeBatch *
provider_next_batch(void *provider_state)
{
	return pg_batch_fdw_scan_next(provider_state);
}

static void
provider_rescan(void *provider_state)
{
	pg_batch_fdw_scan_rescan(provider_state);
}

static void
provider_end_scan(void *provider_state)
{
	pg_batch_fdw_scan_end(provider_state);
}

static void
provider_explain(void *provider_state, ExplainState *es)
{
	pg_batch_fdw_scan_explain(provider_state, es);
}

const PgBatchBridgeProviderOps pg_batch_fdw_provider_ops = {
	.abi_version = PG_BATCH_BRIDGE_ABI_VERSION,
	.struct_size = sizeof(PgBatchBridgeProviderOps),
	.provider_name = PG_BATCH_FDW_PROVIDER_NAME,
	.supports_relation = supports_relation,
	.plan_scan = provider_plan_scan,
	.begin_scan = provider_begin_scan,
	.next_batch = provider_next_batch,
	.rescan = provider_rescan,
	.end_scan = provider_end_scan,
	.explain = provider_explain,
};

static void
get_foreign_rel_size(PlannerInfo *root, RelOptInfo *baserel,
					 Oid relation_oid)
{
	/* The file format deliberately carries no PostgreSQL planner statistics. */
	baserel->rows = 10000;
}

static void
get_foreign_paths(PlannerInfo *root, RelOptInfo *baserel,
				  Oid relation_oid)
{
	Cost		total_cost = seq_page_cost + baserel->rows * cpu_tuple_cost;
	ForeignPath *path;

	path = create_foreignscan_path(root, baserel, NULL, baserel->rows, 0,
								   0, total_cost, NIL, NULL, NULL, NIL, NIL);
	add_path(baserel, &path->path);
}

static ForeignScan *
get_foreign_plan(PlannerInfo *root, RelOptInfo *baserel,
				 Oid relation_oid, ForeignPath *best_path,
				 List *tlist, List *scan_clauses, Plan *outer_plan)
{
	List	   *clauses = NIL;
	List	   *restrictinfos = NIL;
	List	   *source_exprs = NIL;
	List	   *specs = NIL;
	List	   *local_exprs = NIL;
	List	   *remote_exprs = NIL;
	PgBatchBridgeQualSupport *support;
	int			qualno = 0;

	foreach_ptr(RestrictInfo, rinfo, scan_clauses)
	{
		if (rinfo->pseudoconstant)
			continue;
		clauses = lappend(clauses, rinfo->clause);
		restrictinfos = lappend(restrictinfos, rinfo);
	}
	support = palloc0_array(PgBatchBridgeQualSupport, list_length(clauses));
	classify_quals(baserel, clauses, restrictinfos, support,
				   &source_exprs, &specs);
	foreach_ptr(Node, clause, clauses)
	{
		if (support[qualno++] == PG_BATCH_BRIDGE_QUAL_EXACT)
			remote_exprs = lappend(remote_exprs, clause);
		else
			local_exprs = lappend(local_exprs, clause);
	}
	pfree(support);
	list_free(clauses);
	list_free(restrictinfos);
	return make_foreignscan(tlist, local_exprs, baserel->relid,
							source_exprs, specs, NIL, remote_exprs, outer_plan);
}

FdwRoutine *
pg_batch_fdw_routine(void)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	routine->GetForeignRelSize = get_foreign_rel_size;
	routine->GetForeignPaths = get_foreign_paths;
	routine->GetForeignPlan = get_foreign_plan;
	routine->BeginForeignScan = pg_batch_fdw_begin_foreign_scan;
	routine->IterateForeignScan = pg_batch_fdw_iterate_foreign_scan;
	routine->ReScanForeignScan = pg_batch_fdw_rescan_foreign_scan;
	routine->EndForeignScan = pg_batch_fdw_end_foreign_scan;
	routine->ExplainForeignScan = pg_batch_fdw_explain_foreign_scan;
	return routine;
}
