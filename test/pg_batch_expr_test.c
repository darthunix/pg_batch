#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "utils/fmgroids.h"

#include "expr.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_batch_expr_api_test);

static OpExpr *
make_test_op(Oid function, Oid result_type, List *args)
{
	OpExpr	   *op = makeNode(OpExpr);

	op->opfuncid = function;
	op->opresulttype = result_type;
	op->args = args;
	op->location = -1;
	return op;
}

Datum
pg_batch_expr_api_test(PG_FUNCTION_ARGS)
{
	Var		   *column = makeVar(1, 1, INT4OID, -1, InvalidOid, 0);
	Var		   *other_column = makeVar(1, 2, INT4OID, -1, InvalidOid, 0);
	Const	   *three = makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
		Int32GetDatum(3), false, true);
	Const	   *ten = makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
		Int32GetDatum(10), false, true);
	OpExpr	   *sum;
	OpExpr	   *two_columns;
	OpExpr	   *filter;
	AttrNumber	attnum;
	PgBatchInt4Op comparison;
	Node	   *scalar;

	sum = make_test_op(F_INT4PL, INT4OID,
		list_make2(copyObject(column), copyObject(three)));
	two_columns = make_test_op(F_INT4PL, INT4OID,
		list_make2(copyObject(column), copyObject(other_column)));
	filter = make_test_op(F_INT4GT, BOOLOID,
		list_make2(sum, copyObject(ten)));

	PG_RETURN_BOOL(
		pg_batch_expr_supports_int4((Node *) column, 1) &&
		pg_batch_expr_supports_int4((Node *) sum, 1) &&
		!pg_batch_expr_supports_int4((Node *) two_columns, 1) &&
		pg_batch_expr_supports_filter((Node *) filter, 1) &&
		!pg_batch_expr_match_int4_comparison((Node *) filter, 1,
			&attnum, &comparison, &scalar) &&
		pg_batch_expr_match_int4_comparison(
			(Node *) make_test_op(F_INT4GE, BOOLOID,
				list_make2(copyObject(column), copyObject(three))),
			1, &attnum, &comparison, &scalar) &&
		attnum == 1 && comparison == PG_BATCH_INT4_GE &&
		IsA(scalar, Const));
}
