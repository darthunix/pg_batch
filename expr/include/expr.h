/*
 * Restricted batch expressions shared by scan and upper executor nodes.
 *
 * The expression language intentionally contains at most one int4 Var. It
 * supports Const and execution Params, scalar int4 arithmetic, and a final
 * int4 comparison for filters. This keeps execution as a column-to-scalar
 * chain and leaves unsupported PostgreSQL expressions to the scalar executor.
 */
#ifndef PG_BATCH_EXPR_H
#define PG_BATCH_EXPR_H

#include "postgres.h"

#include "executor/executor.h"
#include "nodes/bitmapset.h"
#include "nodes/primnodes.h"

#include "bridge.h"
#include "kernels.h"
#include "runtime.h"

typedef struct PgBatchExpr PgBatchExpr;
typedef struct PgBatchExprProjection PgBatchExprProjection;

/* Map an executor-ready Var to a compact column in the input batch. */
typedef int (*PgBatchExprResolveVar) (const Var *var, void *context);

/* Planning-time checks for the deliberately small expression language. */
extern bool pg_batch_expr_supports_int4(Node *node, Index relid);
extern bool pg_batch_expr_supports_filter(Node *node, Index relid);

/*
 * Recognize the subset useful for storage pruning: a direct int4 Var compared
 * with a scalar expression. scalar is a borrowed pointer into node.
 */
extern bool pg_batch_expr_match_int4_comparison(
	Node *node, Index relid, AttrNumber *attnum,
	PgBatchInt4Op *op, Node **scalar);

/* Compile executor-ready expressions in the caller's current context. */
extern PgBatchExpr *pg_batch_expr_compile_int4(
	Node *node, PlanState *parent, PgBatchExprResolveVar resolve_var,
	void *resolve_context);
extern PgBatchExpr *pg_batch_expr_compile_filter(
	Node *node, PlanState *parent, PgBatchExprResolveVar resolve_var,
	void *resolve_context);

/* Return the only input column, or -1 for a scalar-only expression. */
extern int pg_batch_expr_input_column(const PgBatchExpr *expr);

/* Bind reusable expression state to one active batch. */
extern void pg_batch_expr_bind(
	PgBatchExpr *expr, PgBatchBridgeBatch *batch, ExprContext *econtext,
	PgBatchBridgeMaterializePhase phase);

/*
 * Evaluate a value expression or apply a filter to batch->selection. The
 * caller prepares the expression's input column before either operation.
 */
extern void pg_batch_expr_get_int4(PgBatchExpr *expr,
	PgBatchInt4Vector *result);
extern void pg_batch_expr_apply_filter(PgBatchExpr *expr, bool enable_simd);

/*
 * Compile a list of int4 expressions into a reusable batch projection.
 * Output columns are numbered from zero in list order. Direct Vars keep their
 * input vector representation; computed columns use packed int32 values.
 */
extern PgBatchExprProjection *pg_batch_expr_projection_create(
	MemoryContext parent_context, List *expressions, PlanState *parent,
	PgBatchExprResolveVar resolve_var, void *resolve_context);

/* Bind the projection and return its format-neutral output batch. */
extern PgBatchBridgeBatch *pg_batch_expr_projection_bind(
	PgBatchExprProjection *projection, PgBatchBridgeBatch *input,
	ExprContext *econtext);

/* Borrowed union of compact input columns used by the projection. */
extern const Bitmapset *pg_batch_expr_projection_input_columns(
	const PgBatchExprProjection *projection);

#endif							/* PG_BATCH_EXPR_H */
