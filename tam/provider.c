#include "postgres.h"

#include "catalog/pg_proc.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"

#include "internal.h"

static Node *
strip_relabel(Node *node)
{
	if (IsA(node, RelabelType))
		return (Node *) castNode(RelabelType, node)->arg;
	return node;
}

static bool
is_scalar_operand(Node *node)
{
	return IsA(node, Const) ||
		(IsA(node, Param) &&
		 (castNode(Param, node)->paramkind == PARAM_EXTERN ||
		  castNode(Param, node)->paramkind == PARAM_EXEC));
}

static bool
map_source_operator(Oid funcid, uint8 var_argno,
					SourceOperator *result)
{
	SourceOperator op;

	switch (funcid)
	{
		case F_INT4EQ:
			op = PG_BATCH_TAM_SOURCE_EQ;
			break;
		case F_INT4NE:
			op = PG_BATCH_TAM_SOURCE_NE;
			break;
		case F_INT4LT:
			op = PG_BATCH_TAM_SOURCE_LT;
			break;
		case F_INT4LE:
			op = PG_BATCH_TAM_SOURCE_LE;
			break;
		case F_INT4GT:
			op = PG_BATCH_TAM_SOURCE_GT;
			break;
		case F_INT4GE:
			op = PG_BATCH_TAM_SOURCE_GE;
			break;
		default:
			return false;
	}
	if (var_argno == 1)
	{
		if (op == PG_BATCH_TAM_SOURCE_LT)
			op = PG_BATCH_TAM_SOURCE_GT;
		else if (op == PG_BATCH_TAM_SOURCE_LE)
			op = PG_BATCH_TAM_SOURCE_GE;
		else if (op == PG_BATCH_TAM_SOURCE_GT)
			op = PG_BATCH_TAM_SOURCE_LT;
		else if (op == PG_BATCH_TAM_SOURCE_GE)
			op = PG_BATCH_TAM_SOURCE_LE;
	}
	*result = op;
	return true;
}

static bool
match_source_qual(Node *clause, Index relid, AttrNumber *attnum,
				  SourceOperator *source_op, Node **scalar)
{
	OpExpr	   *op;
	Node	   *args[2];
	uint8		var_argno;
	Var		   *var;

	if (!IsA(clause, OpExpr))
		return false;
	op = castNode(OpExpr, clause);
	if (list_length(op->args) != 2)
		return false;
	args[0] = strip_relabel(linitial(op->args));
	args[1] = strip_relabel(lsecond(op->args));
	if (IsA(args[0], Var) && is_scalar_operand(args[1]))
		var_argno = 0;
	else if (is_scalar_operand(args[0]) && IsA(args[1], Var))
		var_argno = 1;
	else
		return false;
	var = castNode(Var, args[var_argno]);
	if (var->varno != relid || var->varlevelsup != 0 || var->varattno <= 0 ||
		var->varreturningtype != VAR_RETURNING_DEFAULT ||
		!map_source_operator(op->opfuncid, var_argno, source_op))
		return false;
	*attnum = var->varattno;
	*scalar = args[1 - var_argno];
	return true;
}

static bool
supports_relation(Relation relation)
{
	return pg_batch_tam_enable &&
		pg_batch_relation_uses_tableam(relation) &&
		pg_batch_compressed_available(relation);
}

static void
plan_scan(const PgBatchBridgePlanRequest *request,
		  PgBatchBridgePlanResult *result)
{
	List	   *specs = NIL;
	List	   *source_exprs = NIL;
	int			nquals = list_length(request->clauses);
	int			qualno = 0;
	int			exprno = 0;

	MemSet(result, 0, sizeof(*result));
	result->nquals = nquals;
	result->qual_support = palloc0_array(PgBatchBridgeQualSupport, nquals);
	if (pg_batch_tam_scan_mode != PG_BATCH_TAM_BATCH)
	{
		foreach_ptr(Node, clause, request->clauses)
		{
			AttrNumber	attnum;
			SourceOperator source_op;
			Node	   *scalar;

			if (match_source_qual(clause, request->rel->relid,
								  &attnum, &source_op, &scalar))
			{
				source_exprs = lappend(source_exprs, copyObject(scalar));
				specs = lappend(specs,
								list_make3(makeInteger(attnum),
										   makeInteger(source_op),
										   makeInteger(exprno)));
				result->qual_support[qualno] =
					pg_batch_tam_scan_mode == PG_BATCH_TAM_FILTER ?
					PG_BATCH_BRIDGE_QUAL_EXACT :
					PG_BATCH_BRIDGE_QUAL_PRUNE_ONLY;
				exprno++;
			}
			qualno++;
		}
	}
	result->source_exprs = source_exprs;
	result->source_private = (Node *)
		list_make2(makeInteger(pg_batch_tam_scan_mode), specs);
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
begin_scan(const PgBatchBridgeExecRequest *request)
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
		int			exprno = intVal(lthird(spec));
		SourceQual *qual = &scan->quals[qualno++];

		qual->attnum = intVal(linitial(spec));
		qual->op = intVal(lsecond(spec));
		qual->scalar_expr = ExecInitExpr(list_nth(request->source_exprs, exprno),
										 request->parent);
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
	scan->quals_ready = false;
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

const PgBatchBridgeProviderOps pg_batch_tam_provider_ops = {
	.abi_version = PG_BATCH_BRIDGE_ABI_VERSION,
	.struct_size = sizeof(PgBatchBridgeProviderOps),
	.provider_name = PG_BATCH_TAM_PROVIDER_NAME,
	.supports_relation = supports_relation,
	.plan_scan = plan_scan,
	.begin_scan = begin_scan,
	.rescan = rescan,
	.end_scan = end_scan,
	.explain = explain_scan,
};
