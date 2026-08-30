#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "runtime.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_batch_datum_buffer_test);
PG_FUNCTION_INFO_V1(pg_batch_input_api_test);
PG_FUNCTION_INFO_V1(pg_batch_output_test);

Datum
pg_batch_input_api_test(PG_FUNCTION_ARGS)
{
	PgBatchInput *(*volatile create_fn) (MemoryContext,
		const PgBatchAPI *, struct PlanState *, const char *) =
		pg_batch_input_create;
	PgBatchBinding *(*volatile binding_fn) (PgBatchInput *) =
		pg_batch_input_request_binding;
	bool		(*volatile next_fn) (PgBatchInput *, PgBatchInputBatch *) =
		pg_batch_input_next;
	void		(*volatile finish_fn) (PgBatchInput *) = pg_batch_input_finish;
	void		(*volatile rescan_fn) (PgBatchInput *) = pg_batch_input_rescan;

	PG_RETURN_BOOL(create_fn != NULL && binding_fn != NULL &&
				   next_fn != NULL && finish_fn != NULL &&
				   rescan_fn != NULL);
}

static bool
text_matches(Datum value, int row)
{
	char		expected[32];
	char	   *actual = TextDatumGetCString(value);
	bool		matches;

	snprintf(expected, sizeof(expected), "value-%d", row);
	matches = strcmp(actual, expected) == 0;
	pfree(actual);
	return matches;
}

static void
store_test_row(TupleTableSlot *slot, int row)
{
	ExecClearTuple(slot);
	slot->tts_values[0] = Int32GetDatum(row + 1);
	slot->tts_isnull[0] = false;
	if (row % 11 == 0)
	{
		slot->tts_values[1] = (Datum) 0;
		slot->tts_isnull[1] = true;
	}
	else
	{
		slot->tts_values[1] = CStringGetTextDatum(psprintf("value-%d", row));
		slot->tts_isnull[1] = false;
	}
	ExecStoreVirtualTuple(slot);
}

Datum
pg_batch_datum_buffer_test(PG_FUNCTION_ARGS)
{
	MemoryContext test_context;
	MemoryContext row_context;
	MemoryContext oldcontext;
	TupleDesc	desc;
	TupleTableSlot *slot;
	PgBatchDatumBuffer *buffer;
	PgBatch *batch;
	PgBatchDatumVector numbers;
	PgBatchDatumVector strings;
	bool		ok = true;

	test_context = AllocSetContextCreate(CurrentMemoryContext,
										 "pg_batch runtime test",
										 ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(test_context);
	row_context = AllocSetContextCreate(test_context,
										"pg_batch runtime test row",
										ALLOCSET_SMALL_SIZES);
	desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(desc, 1, "number", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 2, "label", TEXTOID, -1, 0);
	TupleDescFinalize(desc);
	desc = BlessTupleDesc(desc);
	slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	buffer = pg_batch_datum_buffer_create(test_context, desc, 2, 70);

	for (int row = 0; row < 70; row++)
	{
		MemoryContext row_oldcontext;

		row_oldcontext = MemoryContextSwitchTo(row_context);
		store_test_row(slot, row);
		MemoryContextSwitchTo(row_oldcontext);
		pg_batch_datum_buffer_append_slot(buffer, slot);
		MemoryContextReset(row_context);
	}
	batch = pg_batch_datum_buffer_finish(buffer, InvalidOid);
	if (batch == NULL || batch->nrows != 70 || batch->nwords != 2 ||
		batch->selection[0] != UINT64_MAX ||
		batch->selection[1] != UINT64CONST(0x3f) ||
		!pg_batch_datum_buffer_is_full(buffer))
		ok = false;
	if (ok)
	{
		batch->ops->get_datum_column(batch, 0, batch->selection,
									PG_BATCH_COLUMN_FILTER,
									&numbers);
		batch->ops->get_datum_column(batch, 1, batch->selection,
									PG_BATCH_COLUMN_PROJECT,
									&strings);
		if (numbers.nwords != 2 || strings.nwords != 2 ||
			numbers.valid_rows[0] != UINT64_MAX ||
			numbers.valid_rows[1] != UINT64CONST(0x3f) ||
			strings.valid_rows[0] != UINT64_MAX ||
			strings.valid_rows[1] != UINT64CONST(0x3f))
			ok = false;
	}
	for (int row = 0; ok && row < 70; row++)
	{
		bool		expected_null = row % 11 == 0;

		if (numbers.isnull[row] || DatumGetInt32(numbers.values[row]) != row + 1 ||
			strings.isnull[row] != expected_null ||
			(!expected_null && !text_matches(strings.values[row], row)))
			ok = false;
	}

	pg_batch_datum_buffer_reset(buffer);
	for (int row = 0; row < 3; row++)
	{
		store_test_row(slot, row + 100);
		pg_batch_datum_buffer_append_slot(buffer, slot);
	}
	batch = pg_batch_datum_buffer_finish(buffer, 42);
	if (batch == NULL || batch->nrows != 3 || batch->nwords != 1 ||
		batch->selection[0] != UINT64CONST(0x7) || batch->table_oid != 42)
		ok = false;

	ExecDropSingleTupleTableSlot(slot);
	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(test_context);
	PG_RETURN_BOOL(ok);
}

Datum
pg_batch_output_test(PG_FUNCTION_ARGS)
{
	MemoryContext context;
	MemoryContext oldcontext;
	TupleDesc	desc;
	TupleTableSlot *input_slot;
	TupleTableSlot *output_slot;
	PgBatchDatumBuffer *buffer;
	PgBatchOutput *output;
	PgBatchBinding *binding;
	PgBatch *batch;
	const PgBatchAPI *api = pg_batch_api_get();
	AttrNumber	attnums[2] = {1, 2};
	bool		ok = true;

	context = AllocSetContextCreate(CurrentMemoryContext,
		"pg_batch output test", ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(context);
	desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(desc, 1, "number", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 2, "label", TEXTOID, -1, 0);
	TupleDescFinalize(desc);
	desc = BlessTupleDesc(desc);
	input_slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	output_slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	buffer = pg_batch_datum_buffer_create(context, desc, 2, 3);
	for (int row = 0; row < 3; row++)
	{
		store_test_row(input_slot, row);
		pg_batch_datum_buffer_append_slot(buffer, input_slot);
	}
	batch = pg_batch_datum_buffer_finish(buffer, 42);
	output = pg_batch_output_create(context, api, output_slot, attnums, 2);
	binding = pg_batch_output_binding(output);
	api->set_request(binding, NULL, NULL, false);
	if (pg_batch_output_publish(output, batch) != output_slot ||
		DatumGetInt32(output_slot->tts_values[0]) != 1 ||
		!output_slot->tts_isnull[1])
		ok = false;
	pg_batch_output_select(output, 2);
	if (DatumGetInt32(output_slot->tts_values[0]) != 3 ||
		output_slot->tts_isnull[1] ||
		!text_matches(output_slot->tts_values[1], 2))
		ok = false;
	pg_batch_output_finish(output);
	if (!api->batch_finished(binding))
		ok = false;
	pg_batch_output_reset(output);
	if (api->get_batch(binding) != NULL || !TupIsNull(output_slot))
		ok = false;
	ExecDropSingleTupleTableSlot(output_slot);
	ExecDropSingleTupleTableSlot(input_slot);
	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(context);
	PG_RETURN_BOOL(ok);
}
