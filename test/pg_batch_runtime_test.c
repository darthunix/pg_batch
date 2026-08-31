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
PG_FUNCTION_INFO_V1(pg_batch_output_double_finish_test);
PG_FUNCTION_INFO_V1(pg_batch_output_fast_select_after_finish_test);
PG_FUNCTION_INFO_V1(pg_batch_output_select_after_finish_test);
PG_FUNCTION_INFO_V1(pg_batch_request_port_test);

Datum
pg_batch_input_api_test(PG_FUNCTION_ARGS)
{
	PgBatchInput *(*volatile create_fn) (MemoryContext,
		const PgBatchAPI *, struct PlanState *, const char *) =
		pg_batch_input_create;
	PgBatchBinding *(*volatile binding_fn) (PgBatchInput *) =
		pg_batch_input_request_binding;
	void		(*volatile request_fn) (PgBatchInput *, const PgBatchRequest *,
		bool) = pg_batch_input_forward_request;
	bool		(*volatile next_fn) (PgBatchInput *, PgBatchInputBatch *) =
		pg_batch_input_next;
	TupleTableSlot *(*volatile select_fn) (PgBatchInput *, int) =
		pg_batch_input_select_row;
	const PgBatchRowView *(*volatile view_fn) (PgBatchInput *) =
		pg_batch_input_row_view;
	bool		(*volatile finished_fn) (PgBatchInput *) =
		pg_batch_input_batch_finished;
	void		(*volatile finish_fn) (PgBatchInput *) = pg_batch_input_finish;
	void		(*volatile rescan_fn) (PgBatchInput *) = pg_batch_input_rescan;

	PG_RETURN_BOOL(create_fn != NULL && binding_fn != NULL &&
				   request_fn != NULL && next_fn != NULL &&
				   select_fn != NULL && view_fn != NULL &&
				   finished_fn != NULL &&
				   finish_fn != NULL &&
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
	PgBatchDatumVector numbers =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchDatumVector);
	PgBatchDatumVector strings =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchDatumVector);
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
	if (ok)
	{
		uint64		keep[2] = {UINT64CONST(0x5), UINT64CONST(0x2)};

		pg_batch_intersect_selection(batch, keep);
		pg_batch_intersect_selection(batch,
			(uint64[2]) {UINT64_MAX, UINT64_MAX});
		if (batch->selection[0] != keep[0] ||
			batch->selection[1] != keep[1])
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
pg_batch_request_port_test(PG_FUNCTION_ARGS)
{
	MemoryContext context;
	MemoryContext oldcontext;
	TupleDesc	desc;
	TupleTableSlot *slot;
	PgBatchRequestPort *port;
	const PgBatchRequest *request;
	const PgBatchAPI *api = pg_batch_api_get();
	AttrNumber	attnums[2] = {3, 7};
	Bitmapset  *filters = bms_make_singleton(0);
	Bitmapset  *projects = bms_make_singleton(1);
	bool		ok;

	context = AllocSetContextCreate(CurrentMemoryContext,
		"pg_batch request port test", ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(context);
	desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(desc, 1, "one", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 2, "two", INT4OID, -1, 0);
	TupleDescFinalize(desc);
	desc = BlessTupleDesc(desc);
	slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	port = pg_batch_request_port_create(context, api, slot, attnums, 2);
	pg_batch_request_port_set_request(port, filters, projects, true);
	request = pg_batch_request_port_request(port);
	ok = pg_batch_request_port_binding(port) == api->find_binding(slot) &&
		request->struct_size >= PG_BATCH_REQUEST_MIN_SIZE &&
		request->ncolumns == 2 && request->source_attnums[0] == 3 &&
		request->source_attnums[1] == 7 && request->return_batch &&
		bms_equal(request->filter_columns, filters) &&
		bms_equal(request->project_columns, projects) &&
		api->get_batch(pg_batch_request_port_binding(port)) == NULL;
	bms_free(filters);
	bms_free(projects);
	ExecDropSingleTupleTableSlot(slot);
	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(context);
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
	PgBatchRowView view = PG_BATCH_STRUCT_INITIALIZER(PgBatchRowView);
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
	api->get_row_view(binding, &view);
	pg_batch_row_view_select(&view, 1);
	if (DatumGetInt32(output_slot->tts_values[0]) != 2 ||
		output_slot->tts_isnull[1] ||
		!text_matches(output_slot->tts_values[1], 1))
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

static PgBatchOutput *
make_active_output(MemoryContext context, const PgBatchAPI *api)
{
	TupleDesc	desc;
	TupleTableSlot *input_slot;
	TupleTableSlot *output_slot;
	PgBatchDatumBuffer *buffer;
	PgBatchOutput *output;
	PgBatch *batch;
	AttrNumber	attnums[2] = {1, 2};

	desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(desc, 1, "number", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 2, "label", TEXTOID, -1, 0);
	TupleDescFinalize(desc);
	desc = BlessTupleDesc(desc);
	input_slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	output_slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	buffer = pg_batch_datum_buffer_create(context, desc, 2, 1);
	store_test_row(input_slot, 0);
	pg_batch_datum_buffer_append_slot(buffer, input_slot);
	batch = pg_batch_datum_buffer_finish(buffer, InvalidOid);
	output = pg_batch_output_create(context, api, output_slot, attnums, 2);
	pg_batch_output_publish(output, batch);
	return output;
}

static PgBatchOutput *
make_finished_output(MemoryContext context, const PgBatchAPI *api)
{
	PgBatchOutput *output = make_active_output(context, api);

	pg_batch_output_finish(output);
	return output;
}

Datum
pg_batch_output_double_finish_test(PG_FUNCTION_ARGS)
{
	const PgBatchAPI *api = pg_batch_api_get();
	MemoryContext context = AllocSetContextCreate(CurrentMemoryContext,
		"pg_batch double finish test", ALLOCSET_DEFAULT_SIZES);
	PgBatchOutput *output = make_finished_output(context, api);

	pg_batch_output_finish(output);
	PG_RETURN_VOID();
}

Datum
pg_batch_output_select_after_finish_test(PG_FUNCTION_ARGS)
{
	const PgBatchAPI *api = pg_batch_api_get();
	MemoryContext context = AllocSetContextCreate(CurrentMemoryContext,
		"pg_batch select after finish test", ALLOCSET_DEFAULT_SIZES);
	PgBatchOutput *output = make_finished_output(context, api);

	pg_batch_output_select(output, 0);
	PG_RETURN_VOID();
}

Datum
pg_batch_output_fast_select_after_finish_test(PG_FUNCTION_ARGS)
{
	const PgBatchAPI *api = pg_batch_api_get();
	MemoryContext context = AllocSetContextCreate(CurrentMemoryContext,
		"pg_batch fast select after finish test", ALLOCSET_DEFAULT_SIZES);
	PgBatchOutput *output = make_active_output(context, api);
	PgBatchRowView view = PG_BATCH_STRUCT_INITIALIZER(PgBatchRowView);

	api->get_row_view(pg_batch_output_binding(output), &view);
	pg_batch_output_finish(output);
	pg_batch_row_view_select(&view, 0);
	PG_RETURN_VOID();
}
