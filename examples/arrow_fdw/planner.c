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
			   PgBatchQualSupport *support,
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
			support[qualno] = PG_BATCH_QUAL_EXACT;
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

static Node *
make_source_plan(List *filter_exprs)
{
	PgBatchPlanWriter *writer = pg_batch_plan_writer_create(
		PG_BATCH_FDW_SOURCE_PLAN_KIND, PG_BATCH_FDW_SOURCE_PLAN_VERSION);

	pg_batch_plan_write_int_list(writer, "filter_exprs", filter_exprs);
	return (Node *) pg_batch_plan_writer_finish(writer);
}

static void
source_plan_scan(const PgBatchSourcePlanRequest *request,
				   PgBatchSourcePlanResult *result)
{
	List	   *source_exprs = NIL;
	List	   *specs = NIL;
	int			nquals;

	if (request->struct_size < PG_BATCH_SOURCE_PLAN_REQUEST_MIN_SIZE ||
		result->struct_size < PG_BATCH_SOURCE_PLAN_RESULT_MIN_SIZE)
		elog(ERROR, "pg_batch_fdw received an incompatible planning request");
	nquals = list_length(request->clauses);

	result->nquals = nquals;
	result->qual_support = palloc0_array(PgBatchQualSupport, nquals);
	classify_quals(request->rel, request->clauses, request->restrictinfos,
				   result->qual_support, &source_exprs, &specs);
	result->source_exprs = source_exprs;
	result->source_private = make_source_plan(specs);
}

static void *
source_begin_scan(const PgBatchSourceExecRequest *request)
{
	if (request->struct_size < PG_BATCH_SOURCE_EXEC_REQUEST_MIN_SIZE)
		elog(ERROR, "pg_batch_fdw received an incompatible execution request");
	return pg_batch_fdw_scan_begin(request->relation, request->parent,
								   request->source_private,
								   request->source_exprs,
								   request->slot_request,
								   request->source_attnums,
								   request->nsource_columns,
								   request->query_context);
}

static PgBatch *
source_next_batch(void *source_state)
{
	return pg_batch_fdw_scan_next(source_state);
}

static void
source_rescan(void *source_state)
{
	pg_batch_fdw_scan_rescan(source_state);
}

static void
source_end_scan(void *source_state)
{
	pg_batch_fdw_scan_end(source_state);
}

static void
source_explain(void *source_state, ExplainState *es)
{
	pg_batch_fdw_scan_explain(source_state, es);
}

const PgBatchSourceOps pg_batch_fdw_source_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_SOURCE_OPS_ABI_VERSION,
		PgBatchSourceOps),
	.source_name = PG_BATCH_FDW_SOURCE_NAME,
	.delivery = PG_BATCH_SOURCE_PULL,
	.supports_relation = supports_relation,
	.plan_scan = source_plan_scan,
	.begin_scan = source_begin_scan,
	.next_batch = source_next_batch,
	.rescan = source_rescan,
	.end_scan = source_end_scan,
	.explain = source_explain,
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
	PgBatchQualSupport *support;
	int			qualno = 0;

	foreach_ptr(RestrictInfo, rinfo, scan_clauses)
	{
		if (rinfo->pseudoconstant)
			continue;
		clauses = lappend(clauses, rinfo->clause);
		restrictinfos = lappend(restrictinfos, rinfo);
	}
	support = palloc0_array(PgBatchQualSupport, list_length(clauses));
	classify_quals(baserel, clauses, restrictinfos, support,
				   &source_exprs, &specs);
	foreach_ptr(Node, clause, clauses)
	{
		if (support[qualno++] == PG_BATCH_QUAL_EXACT)
			remote_exprs = lappend(remote_exprs, clause);
		else
			local_exprs = lappend(local_exprs, clause);
	}
	pfree(support);
	list_free(clauses);
	list_free(restrictinfos);
	return make_foreignscan(tlist, local_exprs, baserel->relid,
							source_exprs, (List *) make_source_plan(specs), NIL,
							remote_exprs, outer_plan);
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
