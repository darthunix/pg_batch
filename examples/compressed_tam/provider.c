#include "postgres.h"

#include "catalog/pg_proc.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "utils/memutils.h"
#include "optimizer/restrictinfo.h"

#include "internal.h"

static bool
supports_relation(Relation relation)
{
	return pg_batch_tam_enable &&
		pg_batch_relation_uses_tableam(relation) &&
		pg_batch_compressed_available(relation);
}

static void
plan_scan(const PgBatchSourcePlanRequest *request,
		  PgBatchSourcePlanResult *result)
{
	List	   *specs = NIL;
	List	   *source_exprs = NIL;
	int			nquals = list_length(request->clauses);
	int			qualno = 0;
	int			exprno = 0;
	ListCell   *clause_cell;
	ListCell   *rinfo_cell;

	MemSet(result, 0, sizeof(*result));
	result->nquals = nquals;
	result->qual_support = palloc0_array(PgBatchQualSupport, nquals);
	if (pg_batch_tam_scan_mode == PG_BATCH_TAM_BATCH)
		goto done;
	if (list_length(request->clauses) != list_length(request->restrictinfos))
		elog(ERROR, "pg_batch TAM received misaligned restrictions");
	forboth(clause_cell, request->clauses, rinfo_cell, request->restrictinfos)
	{
		Node	   *clause = lfirst(clause_cell);
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, rinfo_cell);
		AttrNumber	attnum = InvalidAttrNumber;
		PgBatchInt4Op op = PG_BATCH_INT4_EQ;
		Node	   *scalar = NULL;
		bool		direct;
		bool		exact;
		int			filter_exprno = -1;
		int			scalar_exprno = -1;

		if (!restriction_is_securely_promotable(rinfo, request->rel))
			goto next;
		direct = pg_batch_expr_match_int4_comparison(clause,
			request->rel->relid, &attnum, &op, &scalar);
		exact = pg_batch_tam_scan_mode == PG_BATCH_TAM_FILTER &&
			pg_batch_expr_supports_filter(clause, request->rel->relid);
		if (!direct && !exact)
			goto next;
		if (exact)
		{
			filter_exprno = exprno++;
			source_exprs = lappend(source_exprs, copyObject(clause));
		}
		if (direct)
		{
			scalar_exprno = exprno++;
			source_exprs = lappend(source_exprs, copyObject(scalar));
		}
		specs = lappend(specs,
			list_make5(makeInteger(filter_exprno), makeInteger(attnum),
					   makeInteger(op), makeInteger(scalar_exprno),
					   makeInteger(direct)));
		result->qual_support[qualno] = exact ?
			PG_BATCH_QUAL_EXACT : PG_BATCH_QUAL_PRUNE_ONLY;
next:
		qualno++;
	}
done:
	result->source_exprs = source_exprs;
	result->source_private = (Node *)
		list_make2(makeInteger(pg_batch_tam_scan_mode), specs);
}

static int
resolve_source_var(const Var *var, void *context)
{
	const PgBatchRequest *request = context;

	if (var->varno == INDEX_VAR && var->varattno <= request->ncolumns)
		return var->varattno - 1;
	for (int column = 0; column < request->ncolumns; column++)
	{
		if (request->source_attnums[column] == var->varattno)
			return column;
	}
	return -1;
}

static void
scan_cleanup(void *arg)
{
	CompressedScan *scan = arg;

	if (scan->relation_acquired)
	{
		pg_batch_compressed_release(scan->relation);
		scan->relation_acquired = false;
	}
}

static void *
begin_scan(const PgBatchSourceExecRequest *request)
{
	List	   *private = castNode(List, request->source_private);
	List	   *specs = lsecond_node(List, private);
	MemoryContext context;
	MemoryContext oldcontext;
	CompressedScan *scan;
	int			qualno = 0;

	context = AllocSetContextCreate(request->query_context,
									"pg_batch TAM scan",
									ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(context);
	scan = palloc0_object(CompressedScan);
	scan->context = context;
	scan->batch_context = AllocSetContextCreate(context,
												"pg_batch TAM batch",
												ALLOCSET_DEFAULT_SIZES);
	scan->relation = pg_batch_compressed_acquire(request->relation);
	if (scan->relation == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("compressed snapshot is not available for relation \"%s\"",
						RelationGetRelationName(request->relation))));
	scan->relation_acquired = true;
	scan->cleanup.func = scan_cleanup;
	scan->cleanup.arg = scan;
	MemoryContextRegisterResetCallback(context, &scan->cleanup);
	scan->request = request->slot_request;
	scan->econtext = request->parent->ps_ExprContext;
	scan->mode = intVal(linitial(private));
	scan->nquals = list_length(specs);
	scan->quals = palloc0_array(SourceQual, scan->nquals);
	foreach_ptr(List, spec, specs)
	{
		int			filter_exprno = intVal(linitial(spec));
		int			scalar_exprno = intVal(lfourth(spec));
		SourceQual *qual = &scan->quals[qualno++];

		qual->attnum = intVal(lsecond(spec));
		qual->op = intVal(lthird(spec));
		qual->prunable = intVal(list_nth(spec, 4)) != 0;
		if (filter_exprno >= 0)
		{
			qual->expr = pg_batch_expr_compile_filter(
				list_nth(request->source_exprs, filter_exprno), request->parent,
				resolve_source_var, (void *) request->slot_request);
			qual->column = pg_batch_expr_input_column(qual->expr);
			qual->column_mask = bms_make_singleton(qual->column);
		}
		if (scalar_exprno >= 0)
			qual->scalar_expr = ExecInitExpr(
				list_nth(request->source_exprs, scalar_exprno), request->parent);
	}
	MemoryContextSwitchTo(oldcontext);
	return scan;
}

static void
rescan(void *provider_state)
{
	CompressedScan *scan = provider_state;

	scan->group_index = 0;
	scan->batch_index = 0;
	scan->group_ready = false;
	scan->prune_quals_ready = false;
}

static void
end_scan(void *provider_state)
{
	CompressedScan *scan = provider_state;
	MemoryContext context = scan->context;

	MemoryContextUnregisterResetCallback(context, &scan->cleanup);
	scan_cleanup(scan);
	MemoryContextDelete(context);
}

static void
explain_scan(void *provider_state, ExplainState *es)
{
	CompressedScan *scan = provider_state;
	const char *mode = scan->mode == PG_BATCH_TAM_FILTER ? "filter" :
		scan->mode == PG_BATCH_TAM_PRUNE ? "prune" : "batch";

	ExplainPropertyText("Native Format", "Arrow C Data", es);
	ExplainPropertyText("Source Filter Mode", mode, es);
	if (!es->analyze)
		return;
	ExplainPropertyInteger("Storage Groups Examined", NULL,
						   scan->stats.groups_examined, es);
	ExplainPropertyInteger("Groups Skipped by Min/Max", NULL,
						   scan->stats.groups_skipped_minmax, es);
	ExplainPropertyInteger("Groups Skipped by Membership", NULL,
						   scan->stats.groups_skipped_membership, es);
	ExplainPropertyInteger("Rows Removed by Source Filter", NULL,
						   scan->stats.rows_removed_by_source_filter, es);
	ExplainPropertyInteger("Encoded Bytes Touched", NULL,
						   scan->stats.encoded_bytes_touched, es);
	ExplainPropertyInteger("Arrow Filter Columns", NULL,
						   scan->stats.arrow_filter_columns, es);
	ExplainPropertyInteger("Arrow Projection Columns", NULL,
						   scan->stats.arrow_project_columns, es);
	ExplainPropertyInteger("Arrow Decoded Values", NULL,
						   scan->stats.arrow_decoded_values, es);
	ExplainPropertyInteger("Filter Datums", NULL,
						   scan->stats.filter_datums, es);
	ExplainPropertyInteger("Projection Datums", NULL,
						   scan->stats.project_datums, es);
}

const PgBatchProviderOps pg_batch_tam_provider_ops = {
	.abi_version = PG_BATCH_ABI_VERSION,
	.struct_size = sizeof(PgBatchProviderOps),
	.provider_name = PG_BATCH_TAM_PROVIDER_NAME,
	.supports_relation = supports_relation,
	.plan_scan = plan_scan,
	.begin_scan = begin_scan,
	.rescan = rescan,
	.end_scan = end_scan,
	.explain = explain_scan,
};
