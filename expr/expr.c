#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "utils/fmgroids.h"

#include "expr.h"

typedef struct ArithmeticStep
{
	PgBatchInt4ArithmeticOp op;
	ExprState  *scalar;
	bool		scalar_on_left;
} ArithmeticStep;

struct PgBatchExpr
{
	MemoryContext context;
	int			column;
	ExprState  *scalar_value;
	ArithmeticStep *steps;
	int			nsteps;
	ExprState  *comparison_scalar;
	PgBatchInt4Op comparison;
	bool		is_filter;

	PgBatch *batch;
	ExprContext *econtext;
	PgBatchColumnPhase phase;
	int			capacity;
	int32	   *values[2];
	uint8	   *validity[2];
	PgBatchInt4Vector result;
	bool		result_ready;
};

static Node *
strip_relabel(Node *node)
{
	while (IsA(node, RelabelType))
		node = (Node *) castNode(RelabelType, node)->arg;
	return node;
}

static bool
valid_var(const Var *var, Index relid)
{
	return var->vartype == INT4OID && var->varattno > 0 &&
		var->varlevelsup == 0 &&
		var->varreturningtype == VAR_RETURNING_DEFAULT &&
		(relid == 0 || var->varno == relid);
}

static bool
scalar_leaf(Node *node)
{
	if (IsA(node, Const))
		return castNode(Const, node)->consttype == INT4OID;
	if (IsA(node, Param))
	{
		Param	   *param = castNode(Param, node);

		return param->paramtype == INT4OID &&
			(param->paramkind == PARAM_EXTERN ||
			 param->paramkind == PARAM_EXEC);
	}
	return false;
}

static bool
arithmetic_op(Oid funcid, int nargs, PgBatchInt4ArithmeticOp *result)
{
	switch (funcid)
	{
		case F_INT4PL:
			*result = PG_BATCH_INT4_ADD;
			break;
		case F_INT4MI:
			*result = PG_BATCH_INT4_SUBTRACT;
			break;
		case F_INT4MUL:
			*result = PG_BATCH_INT4_MULTIPLY;
			break;
		case F_INT4DIV:
			*result = PG_BATCH_INT4_DIVIDE;
			break;
		case F_INT4MOD:
			*result = PG_BATCH_INT4_MODULO;
			break;
		case F_INT4UM:
			if (nargs != 1)
				return false;
			*result = PG_BATCH_INT4_NEGATE;
			return true;
		default:
			return false;
	}
	return nargs == 2;
}

/* Return the number of Vars, stopping once the restricted grammar fails. */
static bool
analyze_value(Node *node, Index relid, int *nvars)
{
	PgBatchInt4ArithmeticOp ignored;
	OpExpr	   *op;
	int			vars = 0;

	node = strip_relabel(node);
	if (IsA(node, Var))
	{
		if (!valid_var(castNode(Var, node), relid))
			return false;
		*nvars = 1;
		return true;
	}
	if (scalar_leaf(node))
	{
		*nvars = 0;
		return true;
	}
	if (!IsA(node, OpExpr))
		return false;
	op = castNode(OpExpr, node);
	if (exprType(node) != INT4OID ||
		!arithmetic_op(op->opfuncid, list_length(op->args), &ignored))
		return false;
	foreach_ptr(Node, arg, op->args)
	{
		int			arg_vars;

		if (!analyze_value(arg, relid, &arg_vars))
			return false;
		vars += arg_vars;
		if (vars > 1)
			return false;
	}
	*nvars = vars;
	return true;
}

bool
pg_batch_expr_supports_int4(Node *node, Index relid)
{
	int			nvars;

	return analyze_value(node, relid, &nvars);
}

static bool
analyze_filter(Node *node, Index relid, int *value_arg,
			   PgBatchInt4Op *comparison)
{
	OpExpr	   *op;
	int			left_vars;
	int			right_vars;

	node = strip_relabel(node);
	if (!IsA(node, OpExpr))
		return false;
	op = castNode(OpExpr, node);
	if (list_length(op->args) != 2 || exprType(node) != BOOLOID ||
		!analyze_value(linitial(op->args), relid, &left_vars) ||
		!analyze_value(lsecond(op->args), relid, &right_vars) ||
		left_vars + right_vars != 1)
		return false;
	*value_arg = left_vars == 1 ? 0 : 1;
	return pg_batch_int4_compare_op(op->opfuncid, *value_arg, comparison);
}

bool
pg_batch_expr_supports_filter(Node *node, Index relid)
{
	PgBatchInt4Op comparison;
	int			value_arg;

	return analyze_filter(node, relid, &value_arg, &comparison);
}

bool
pg_batch_expr_match_int4_comparison(Node *node, Index relid,
									AttrNumber *attnum,
									PgBatchInt4Op *comparison,
									Node **scalar)
{
	OpExpr	   *op;
	Node	   *args[2];
	int			value_arg;
	int			nvars;

	node = strip_relabel(node);
	if (!analyze_filter(node, relid, &value_arg, comparison))
		return false;
	op = castNode(OpExpr, node);
	args[0] = strip_relabel(linitial(op->args));
	args[1] = strip_relabel(lsecond(op->args));
	if (!IsA(args[value_arg], Var) ||
		!analyze_value(args[1 - value_arg], relid, &nvars) || nvars != 0)
		return false;
	*attnum = castNode(Var, args[value_arg])->varattno;
	*scalar = args[1 - value_arg];
	return true;
}

static void
append_step(PgBatchExpr *expr, PgBatchInt4ArithmeticOp op, Node *scalar,
			bool scalar_on_left, PlanState *parent)
{
	ArithmeticStep *step;

	if (expr->steps == NULL)
		expr->steps = palloc(sizeof(ArithmeticStep));
	else
		expr->steps = repalloc_array(expr->steps, ArithmeticStep,
									expr->nsteps + 1);
	step = &expr->steps[expr->nsteps++];
	step->op = op;
	step->scalar = scalar == NULL ? NULL :
		ExecInitExpr((Expr *) scalar, parent);
	step->scalar_on_left = scalar_on_left;
}

static void
compile_value(PgBatchExpr *expr, Node *node, PlanState *parent,
			  PgBatchExprResolveVar resolve_var, void *resolve_context)
{
	OpExpr	   *op;
	PgBatchInt4ArithmeticOp arithmetic;
	int			left_vars;
	int			right_vars = 0;

	node = strip_relabel(node);
	if (IsA(node, Var))
	{
		expr->column = resolve_var(castNode(Var, node), resolve_context);
		if (expr->column < 0)
			elog(ERROR, "pg_batch expression input column is unavailable");
		return;
	}
	if (!analyze_value(node, 0, &left_vars))
		elog(ERROR, "pg_batch received an unsupported int4 expression");
	if (left_vars == 0)
	{
		expr->scalar_value = ExecInitExpr((Expr *) node, parent);
		return;
	}

	op = castNode(OpExpr, node);
	if (!arithmetic_op(op->opfuncid, list_length(op->args), &arithmetic))
		elog(ERROR, "pg_batch received an invalid int4 arithmetic expression");
	if (list_length(op->args) == 1)
	{
		compile_value(expr, linitial(op->args), parent,
					  resolve_var, resolve_context);
		append_step(expr, arithmetic, NULL, false, parent);
		return;
	}
	(void) analyze_value(linitial(op->args), 0, &left_vars);
	(void) analyze_value(lsecond(op->args), 0, &right_vars);
	if (left_vars == 1)
	{
		compile_value(expr, linitial(op->args), parent,
					  resolve_var, resolve_context);
		append_step(expr, arithmetic, lsecond(op->args), false, parent);
	}
	else
	{
		Assert(right_vars == 1);
		compile_value(expr, lsecond(op->args), parent,
					  resolve_var, resolve_context);
		append_step(expr, arithmetic, linitial(op->args), true, parent);
	}
}

static PgBatchExpr *
compile_expression(Node *node, bool filter, PlanState *parent,
				   PgBatchExprResolveVar resolve_var, void *resolve_context)
{
	PgBatchExpr *expr = palloc0_object(PgBatchExpr);
	Node	   *value = node;

	expr->context = CurrentMemoryContext;
	expr->column = -1;
	expr->is_filter = filter;
	if (filter)
	{
		OpExpr	   *op;
		int			value_arg;

		if (!analyze_filter(node, 0, &value_arg, &expr->comparison))
			elog(ERROR, "pg_batch received an unsupported filter expression");
		op = castNode(OpExpr, strip_relabel(node));
		value = list_nth(op->args, value_arg);
		expr->comparison_scalar = ExecInitExpr(
			(Expr *) list_nth(op->args, 1 - value_arg), parent);
	}
	else if (!pg_batch_expr_supports_int4(node, 0))
		elog(ERROR, "pg_batch received an unsupported int4 expression");
	compile_value(expr, value, parent, resolve_var, resolve_context);
	return expr;
}

PgBatchExpr *
pg_batch_expr_compile_int4(Node *node, PlanState *parent,
						   PgBatchExprResolveVar resolve_var,
						   void *resolve_context)
{
	return compile_expression(node, false, parent, resolve_var, resolve_context);
}

PgBatchExpr *
pg_batch_expr_compile_filter(Node *node, PlanState *parent,
							PgBatchExprResolveVar resolve_var,
							void *resolve_context)
{
	return compile_expression(node, true, parent, resolve_var, resolve_context);
}

int
pg_batch_expr_input_column(const PgBatchExpr *expr)
{
	return expr->column;
}

void
pg_batch_expr_bind(PgBatchExpr *expr, PgBatch *batch,
				   ExprContext *econtext,
				   PgBatchColumnPhase phase)
{
	expr->batch = batch;
	expr->econtext = econtext;
	expr->phase = phase;
	expr->result_ready = false;
}

static void
ensure_capacity(PgBatchExpr *expr)
{
	MemoryContext oldcontext;

	if (expr->capacity >= expr->batch->nrows)
		return;
	oldcontext = MemoryContextSwitchTo(expr->context);
	for (int i = 0; i < (expr->nsteps > 1 ? 2 : 1); i++)
	{
		if (expr->values[i] == NULL)
		{
			expr->values[i] = palloc_array(int32, expr->batch->nrows);
			expr->validity[i] = palloc0((expr->batch->nrows + 7) / 8);
		}
		else
		{
			expr->values[i] = repalloc_array(expr->values[i], int32,
										expr->batch->nrows);
			expr->validity[i] = repalloc0(expr->validity[i],
									   (expr->capacity + 7) / 8,
									   (expr->batch->nrows + 7) / 8);
		}
	}
	expr->capacity = expr->batch->nrows;
	MemoryContextSwitchTo(oldcontext);
}

static void
build_scalar_vector(PgBatchExpr *expr, PgBatchInt4Vector *result)
{
	bool		isnull;
	Datum		value;

	ensure_capacity(expr);
	MemSet(expr->validity[0], 0, (expr->batch->nrows + 7) / 8);
	if (!pg_batch_has_rows(expr->batch))
	{
		pg_batch_int4_vector_init_packed(result, expr->values[0],
									 expr->validity[0], 0);
		return;
	}
	value = ExecEvalExpr(expr->scalar_value, expr->econtext, &isnull);
	if (!isnull)
	{
		for (int word = 0; word < expr->batch->nwords; word++)
		{
			uint64		rows = pg_batch_selection_word(expr->batch, word);

			while (rows != 0)
			{
				int			bitno = pg_rightmost_one_pos64(rows);
				int			row = word * 64 + bitno;

				expr->values[0][row] = DatumGetInt32(value);
				expr->validity[0][row / 8] |= (uint8) 1 << (row % 8);
				rows &= rows - 1;
			}
		}
	}
	pg_batch_int4_vector_init_packed(result, expr->values[0],
								 expr->validity[0], 0);
}

void
pg_batch_expr_get_int4(PgBatchExpr *expr, PgBatchInt4Vector *result)
{
	PgBatchInt4Vector input;
	int			buffer = 0;

	if (expr->batch == NULL || expr->econtext == NULL)
		elog(ERROR, "pg_batch expression is not bound to a batch");
	if (expr->result_ready)
	{
		*result = expr->result;
		return;
	}
	if (expr->scalar_value != NULL)
		build_scalar_vector(expr, &input);
	else
	{
		pg_batch_get_int4_vector(expr->batch, expr->column,
							 expr->batch->selection, expr->phase, &input);
	}

	if (expr->nsteps > 0)
		ensure_capacity(expr);
	for (int i = 0; i < expr->nsteps; i++)
	{
		ArithmeticStep *step = &expr->steps[i];
		Datum		scalar_value = Int32GetDatum(0);
		bool		scalar_isnull = false;

		if (step->scalar != NULL && pg_batch_has_rows(expr->batch))
			scalar_value = ExecEvalExpr(step->scalar, expr->econtext,
									&scalar_isnull);
		if (scalar_isnull)
			MemSet(expr->validity[buffer], 0,
				   (expr->batch->nrows + 7) / 8);
		else
			pg_batch_int4_arithmetic_scalar(&input, expr->batch->nrows,
										 expr->batch->nwords,
										 expr->batch->selection, step->op,
										 DatumGetInt32(scalar_value),
										 step->scalar_on_left,
										 expr->values[buffer],
										 expr->validity[buffer]);
		pg_batch_int4_vector_init_packed(&input, expr->values[buffer],
									 expr->validity[buffer], 0);
		buffer = 1 - buffer;
	}
	expr->result = input;
	expr->result_ready = true;
	*result = input;
}

void
pg_batch_expr_apply_filter(PgBatchExpr *expr, bool enable_simd)
{
	PgBatchInt4Vector value;
	Datum		scalar;
	bool		isnull;

	if (!expr->is_filter)
		elog(ERROR, "pg_batch value expression cannot be used as a filter");
	if (!pg_batch_has_rows(expr->batch))
		return;
	pg_batch_expr_get_int4(expr, &value);
	scalar = ExecEvalExpr(expr->comparison_scalar, expr->econtext, &isnull);
	if (isnull)
	{
		MemSet(expr->batch->selection, 0,
			   sizeof(uint64) * expr->batch->nwords);
		return;
	}
	pg_batch_filter_int4(&value, expr->batch->nrows, expr->batch->nwords,
						 expr->batch->selection, expr->comparison,
						 DatumGetInt32(scalar), enable_simd);
}
