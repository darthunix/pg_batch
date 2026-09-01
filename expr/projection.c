#include "postgres.h"

#include "port/pg_bitutils.h"

#include "expr.h"

typedef struct ProjectionColumn
{
	PgBatchExpr *expr;
	Bitmapset  *input_column;
	PgBatchInt4Vector vector;
	Datum	   *datum_values;
	bool	   *datum_nulls;
	uint64	   *datum_rows;
	int			datum_capacity;
	int			datum_nword_capacity;
	bool		vector_ready;
} ProjectionColumn;

struct PgBatchExprProjection
{
	MemoryContext context;
	PgBatch batch;
	PgBatch *input;
	ProjectionColumn *columns;
	Bitmapset  *input_columns;
	int			ncolumns;
};

static void
check_column(PgBatchExprProjection *projection, int column)
{
	if (column < 0 || column >= projection->ncolumns)
		elog(ERROR, "pg_batch projection column %d is out of range", column + 1);
}

static void
prepare_one(PgBatchExprProjection *projection, int column,
			const PgBatchSelection *rows, PgBatchColumnPhase phase)
{
	ProjectionColumn *output;

	check_column(projection, column);
	output = &projection->columns[column];
	if (output->vector_ready)
		return;
	pg_batch_prepare_columns(projection->input, output->input_column,
							 rows, phase);
	pg_batch_expr_get_int4(output->expr, &output->vector);
	output->vector_ready = true;
}

static void
projection_prepare_columns(PgBatch *batch,
						   const Bitmapset *columns,
						   const PgBatchSelection *rows,
						   PgBatchColumnPhase phase)
{
	PgBatchExprProjection *projection = batch->private_data;
	int			column = -1;

	while (columns != NULL &&
		   (column = bms_next_member(columns, column)) >= 0)
		prepare_one(projection, column, rows, phase);
}

static bool
projection_get_int4(PgBatch *batch, int column,
					PgBatchInt4Vector *result)
{
	PgBatchExprProjection *projection = batch->private_data;
	ProjectionColumn *output;

	prepare_one(projection, column, &batch->selection,
				PG_BATCH_COLUMN_PROJECT);
	output = &projection->columns[column];
	*result = output->vector;
	return true;
}

static const PgBatchInt4VectorInterface projection_int4_interface = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_INT4_VECTOR_INTERFACE_VERSION,
		PgBatchInt4VectorInterface),
	.get_column = projection_get_int4,
};

static const PgBatchNativeInterface *
projection_get_native_interface(const PgBatchNativeType *type)
{
	if (type->abi_version == PG_BATCH_INT4_VECTOR_INTERFACE_VERSION &&
		strcmp(type->name, PG_BATCH_INT4_VECTOR_INTERFACE_NAME) == 0)
		return (const PgBatchNativeInterface *) &projection_int4_interface;
	return NULL;
}

static void
ensure_datum_capacity(PgBatchExprProjection *projection,
					  ProjectionColumn *output)
{
	MemoryContext oldcontext;
	int			nrows = projection->batch.selection.nrows;
	int			nwords = projection->batch.selection.nwords;

	if (output->datum_capacity >= nrows &&
		output->datum_nword_capacity >= nwords)
		return;
	oldcontext = MemoryContextSwitchTo(projection->context);
	if (output->datum_values == NULL)
	{
		output->datum_values = palloc_array(Datum, nrows);
		output->datum_nulls = palloc_array(bool, nrows);
		output->datum_rows = palloc0_array(uint64, nwords);
	}
	else
	{
		output->datum_values = repalloc_array(output->datum_values,
			Datum, nrows);
		output->datum_nulls = repalloc_array(output->datum_nulls,
			bool, nrows);
		output->datum_rows = repalloc0_array(output->datum_rows,
			uint64, output->datum_nword_capacity, nwords);
	}
	output->datum_capacity = Max(output->datum_capacity, nrows);
	output->datum_nword_capacity = Max(output->datum_nword_capacity, nwords);
	MemoryContextSwitchTo(oldcontext);
}

static void
projection_get_datum_column(PgBatch *batch, int column,
							const PgBatchSelection *rows,
							PgBatchColumnPhase phase,
							PgBatchDatumVector *result)
{
	PgBatchExprProjection *projection = batch->private_data;
	ProjectionColumn *output;

	pg_batch_check_datum_vector(result);
	prepare_one(projection, column, rows, phase);
	output = &projection->columns[column];
	ensure_datum_capacity(projection, output);
	for (int word = 0; word < batch->selection.nwords; word++)
	{
		uint64		missing = rows->words[word] & ~output->datum_rows[word];

		while (missing != 0)
		{
			int			bitno = pg_rightmost_one_pos64(missing);
			uint64		bit = UINT64CONST(1) << bitno;
			int			row = word * 64 + bitno;
			bool		isnull = pg_batch_int4_row_is_null(&output->vector, row);

			output->datum_values[row] = isnull ? (Datum) 0 :
				Int32GetDatum(pg_batch_int4_row_value(&output->vector, row));
			output->datum_nulls[row] = isnull;
			output->datum_rows[word] |= bit;
			missing &= ~bit;
		}
	}
	result->values = output->datum_values;
	result->isnull = output->datum_nulls;
	result->nrows = batch->selection.nrows;
}

static const PgBatchOps projection_batch_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_OPS_ABI_VERSION, PgBatchOps),
	.prepare_columns = projection_prepare_columns,
	.get_datum_column = projection_get_datum_column,
	.get_native_interface = projection_get_native_interface,
};

PgBatchExprProjection *
pg_batch_expr_projection_create(MemoryContext parent_context,
								List *expressions, PlanState *parent,
								PgBatchExprResolveVar resolve_var,
								void *resolve_context)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(parent_context);
	PgBatchExprProjection *projection =
		palloc0_object(PgBatchExprProjection);
	int			column = 0;

	projection->context = parent_context;
	projection->ncolumns = list_length(expressions);
	projection->columns = palloc0_array(ProjectionColumn,
										 projection->ncolumns);
	foreach_ptr(Node, expression, expressions)
	{
		ProjectionColumn *output = &projection->columns[column++];
		int			input_column;

		output->expr = pg_batch_expr_compile_int4(expression, parent,
			resolve_var, resolve_context);
		input_column = pg_batch_expr_input_column(output->expr);
		if (input_column >= 0)
		{
			output->input_column = bms_make_singleton(input_column);
			projection->input_columns =
				bms_add_member(projection->input_columns, input_column);
		}
	}
	projection->batch.abi_version = PG_BATCH_ABI_VERSION;
	projection->batch.struct_size = sizeof(PgBatch);
	projection->batch.ops = &projection_batch_ops;
	projection->batch.private_data = projection;
	MemoryContextSwitchTo(oldcontext);
	return projection;
}

PgBatch *
pg_batch_expr_projection_bind(PgBatchExprProjection *projection,
							  PgBatch *input,
							  ExprContext *econtext)
{
	projection->input = input;
	for (int column = 0; column < projection->ncolumns; column++)
	{
		ProjectionColumn *output = &projection->columns[column];

		output->vector_ready = false;
		if (output->datum_rows != NULL)
			MemSet(output->datum_rows, 0,
				   sizeof(uint64) * output->datum_nword_capacity);
		pg_batch_expr_bind(output->expr, input, econtext,
						   PG_BATCH_COLUMN_PROJECT);
	}
	projection->batch.selection.nrows = input->selection.nrows;
	projection->batch.selection.nwords = input->selection.nwords;
	projection->batch.selection.words = input->selection.words;
	projection->batch.table_oid = input->table_oid;
	return &projection->batch;
}

const Bitmapset *
pg_batch_expr_projection_input_columns(
	const PgBatchExprProjection *projection)
{
	return projection->input_columns;
}
