#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#include "runtime.h"
#include "plan.h"
#include "planner.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_batch_builder_test);
PG_FUNCTION_INFO_V1(pg_batch_input_api_test);
PG_FUNCTION_INFO_V1(pg_batch_output_test);
PG_FUNCTION_INFO_V1(pg_batch_output_double_finish_test);
PG_FUNCTION_INFO_V1(pg_batch_output_fast_select_after_finish_test);
PG_FUNCTION_INFO_V1(pg_batch_output_over_max_test);
PG_FUNCTION_INFO_V1(pg_batch_output_select_after_finish_test);
PG_FUNCTION_INFO_V1(pg_batch_binding_request_test);
PG_FUNCTION_INFO_V1(pg_batch_request_after_seal_test);
PG_FUNCTION_INFO_V1(pg_batch_plan_data_test);
PG_FUNCTION_INFO_V1(pg_batch_planner_api_test);
PG_FUNCTION_INFO_V1(pg_batch_planner_invalid_test);

static const CustomPathMethods planner_test_path_methods = {
	.CustomName = "PgBatchPlannerTest",
};

static const CustomPathMethods planner_test_child_path_methods = {
	.CustomName = "PgBatchPlannerTestChild",
};

static const CustomScanMethods planner_test_scan_methods = {
	.CustomName = "PgBatchPlannerTest",
};

static void
planner_test_child_layout(const Plan *plan, PgBatchLayout *result)
{
	static const int columns[] = {2, 0};

	(void) plan;
	result->ncolumns = 3;
	result->ntargets = 2;
	result->target_columns = columns;
}

static const PgBatchNodeOps planner_test_child_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_NODE_OPS_ABI_VERSION, PgBatchNodeOps),
	.node_name = "pg_batch_test.child",
	.get_layout = planner_test_child_layout,
};

static const PgBatchNodeOps *
planner_test_find_node(const Path *path)
{
	if (pg_batch_path_matches(path, &planner_test_child_path_methods))
		return &planner_test_child_ops;
	return NULL;
}

static TargetEntry *
planner_test_target(int resno)
{
	Const	   *value = makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
		Int32GetDatum(resno), false, true);

	return makeTargetEntry((Expr *) value, resno, NULL, false);
}

Datum
pg_batch_planner_api_test(PG_FUNCTION_ARGS)
{
	Path		template = {0};
	CustomPath *child_path;
	CustomScan *child_plan = makeNode(CustomScan);
	CustomPath *path;
	PgBatchPathConfig path_config =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPathConfig);
	PgBatchPathInfo path_info =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPathInfo);
	PgBatchPlanConfig plan_config =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanConfig);
	PgBatchPlanInfo plan_info =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanInfo);
	PgBatchAPI api = {
		PG_BATCH_ABI_INITIALIZER(PG_BATCH_API_ABI_VERSION, PgBatchAPI),
		.find_node = planner_test_find_node,
	};
	List	   *targetlist = list_make2(planner_test_target(1),
		planner_test_target(2));
	List	   *expressions = list_make1(makeInteger(7));
	Plan	   *plan;
	int			explicit_columns[] = {1, 0};
	PgBatchLayout explicit_layout =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);
	bool		ok;

	child_path = makeNode(CustomPath);
	child_path->methods = &planner_test_child_path_methods;
	child_plan->scan.plan.targetlist = copyObject(targetlist);
	path_config.template_path = &template;
	path_config.methods = &planner_test_path_methods;
	path_config.children = list_make1(&child_path->path);
	path_config.expressions = expressions;
	path_config.node_data = (Node *) makeString(pstrdup("path data"));
	path = pg_batch_path_create(&path_config);
	pg_batch_path_get_info(path, &path_info);
	ok = pg_batch_path_matches(&path->path, &planner_test_path_methods) &&
		equal(path_info.expressions, expressions) &&
		IsA(path_info.node_data, String) &&
		strcmp(strVal(path_info.node_data), "path data") == 0;

	plan_config.api = &api;
	plan_config.methods = &planner_test_scan_methods;
	plan_config.layout_policy = PG_BATCH_LAYOUT_PRESERVE_CHILD;
	plan_config.layout_child = 0;
	plan_config.expressions = path_info.expressions;
	plan_config.node_data = (Node *) makeString(pstrdup("plan data"));
	plan = pg_batch_plan_create(NULL, path, targetlist,
		list_make1(&child_plan->scan.plan), &plan_config);
	pg_batch_plan_get_info(castNode(CustomScan, plan), &plan_info);
	ok = ok && plan_info.nchildren == 1 &&
		plan_info.child_names[0] != NULL &&
		strcmp(plan_info.child_names[0], "pg_batch_test.child") == 0 &&
		plan_info.layout.ncolumns == 3 && plan_info.layout.ntargets == 2 &&
		pg_batch_layout_column(&plan_info.layout, 0) == 2 &&
		pg_batch_layout_column(&plan_info.layout, 1) == 0 &&
		IsA(plan_info.node_data, String) &&
		strcmp(strVal(plan_info.node_data), "plan data") == 0;
	pfree(plan_info.child_names);
	pfree((void *) plan_info.layout.target_columns);

	/* A dense layout follows a target list changed after PlanCustomPath. */
	path_config.children = NIL;
	path = pg_batch_path_create(&path_config);
	plan_config.layout_policy = PG_BATCH_LAYOUT_DENSE;
	plan_config.expressions = NIL;
	plan_config.node_data = NULL;
	plan = pg_batch_plan_create(NULL, path, targetlist, NIL, &plan_config);
	plan->targetlist = lappend(plan->targetlist, planner_test_target(3));
	plan_info = (PgBatchPlanInfo)
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanInfo);
	pg_batch_plan_get_info(castNode(CustomScan, plan), &plan_info);
	ok = ok && plan_info.nchildren == 0 &&
		plan_info.layout.ncolumns == 3 && plan_info.layout.ntargets == 3 &&
		plan_info.layout.target_columns == NULL;
	pfree(plan_info.child_names);

	explicit_layout.ncolumns = 2;
	explicit_layout.ntargets = 2;
	explicit_layout.target_columns = explicit_columns;
	plan_config.layout_policy = PG_BATCH_LAYOUT_EXPLICIT;
	plan_config.explicit_layout = &explicit_layout;
	plan = pg_batch_plan_create(NULL, path, targetlist, NIL, &plan_config);
	plan_info = (PgBatchPlanInfo)
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanInfo);
	pg_batch_plan_get_info(castNode(CustomScan, plan), &plan_info);
	ok = ok && plan_info.layout.ncolumns == 2 &&
		pg_batch_layout_column(&plan_info.layout, 0) == 1 &&
		pg_batch_layout_column(&plan_info.layout, 1) == 0;

	PG_RETURN_BOOL(ok);
}

Datum
pg_batch_planner_invalid_test(PG_FUNCTION_ARGS)
{
	int			mode = PG_GETARG_INT32(0);
	Path		template = {0};
	CustomPath *child_path = makeNode(CustomPath);
	CustomScan *child_plan = makeNode(CustomScan);
	CustomPath *path;
	PgBatchPathConfig path_config =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPathConfig);
	PgBatchPlanConfig plan_config =
		PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanConfig);
	PgBatchAPI api = {
		PG_BATCH_ABI_INITIALIZER(PG_BATCH_API_ABI_VERSION, PgBatchAPI),
		.find_node = planner_test_find_node,
	};
	List	   *targetlist = list_make1(planner_test_target(1));

	if (mode != 0 && mode != 1)
		elog(ERROR, "invalid pg_batch planner test mode");
	child_path->methods = &planner_test_path_methods;
	child_plan->scan.plan.targetlist = copyObject(targetlist);
	path_config.template_path = &template;
	path_config.methods = &planner_test_path_methods;
	path_config.children = mode == 0 ? NIL :
		list_make1(&child_path->path);
	path = pg_batch_path_create(&path_config);
	plan_config.api = &api;
	plan_config.methods = &planner_test_scan_methods;
	plan_config.layout_policy = PG_BATCH_LAYOUT_PRESERVE_CHILD;
	plan_config.layout_child = 0;
	(void) pg_batch_plan_create(NULL, path, targetlist,
		list_make1(&child_plan->scan.plan), &plan_config);
	PG_RETURN_VOID();
}

Datum
pg_batch_plan_data_test(PG_FUNCTION_ARGS)
{
	PgBatchPlanWriter *writer = pg_batch_plan_writer_create("test", 1);
	PgBatchPlanReader *reader;
	List	   *data;
	List	   *items = list_make3_int(2, 5, 9);
	Bitmapset  *columns = bms_add_member(NULL, 7);
	Bitmapset  *decoded;
	bool		ok;

	columns = bms_add_member(columns, 11);
	pg_batch_plan_write_int_list(writer, "items", items);
	pg_batch_plan_write_string(writer, "name", "example");
	pg_batch_plan_write_int(writer, "count", 42);
	pg_batch_plan_write_bitmap(writer, "columns", columns);
	data = copyObject(pg_batch_plan_writer_finish(writer));
	reader = pg_batch_plan_reader_create(data, "test", 1);
	ok = pg_batch_plan_read_int(reader, "count") == 42 &&
		strcmp(pg_batch_plan_read_string(reader, "name"), "example") == 0 &&
		equal(pg_batch_plan_read_int_list(reader, "items"), items);
	decoded = pg_batch_plan_read_bitmap(reader, "columns");
	ok = ok && bms_equal(decoded, columns);
	pg_batch_plan_reader_finish(reader);
	PG_RETURN_BOOL(ok);
}

static void
accept_nothing(void *consumer_state, TupleTableSlot *slot, PgBatch *batch)
{
	(void) consumer_state;
	(void) slot;
	(void) batch;
}

static void
select_nothing(void *consumer_state, TupleTableSlot *slot,
			   PgBatch *batch, int row)
{
	(void) consumer_state;
	(void) slot;
	(void) batch;
	(void) row;
}

static const PgBatchConsumerOps test_consumer_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_CONSUMER_OPS_ABI_VERSION,
		PgBatchConsumerOps),
	.accept_batch = accept_nothing,
	.select_row = select_nothing,
};

Datum
pg_batch_input_api_test(PG_FUNCTION_ARGS)
{
	PgBatchInput *(*volatile create_fn) (MemoryContext,
		struct PlanState *, const char *) =
		pg_batch_input_create;
	PgBatchBinding *(*volatile binding_fn) (PgBatchInput *) =
		pg_batch_input_request_binding;
	void		(*volatile request_fn) (PgBatchInput *,
		const PgBatchRequestSpec *) = pg_batch_input_set_request;
	bool		(*volatile next_fn) (PgBatchInput *, PgBatchInputResult *) =
		pg_batch_input_next;
	TupleTableSlot *(*volatile select_fn) (PgBatchInput *, int) =
		pg_batch_input_select_row;
	const PgBatchRowView *(*volatile view_fn) (PgBatchInput *) =
		pg_batch_input_row_view;
	bool		(*volatile finished_fn) (PgBatchInput *) =
		pg_batch_input_finished;
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
pg_batch_builder_test(PG_FUNCTION_ARGS)
{
	MemoryContext test_context;
	MemoryContext row_context;
	MemoryContext oldcontext;
	TupleDesc	desc;
	TupleTableSlot *slot;
	PgBatchBuilder *buffer;
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
	buffer = pg_batch_builder_create(
		&(PgBatchBuilderConfig) {
			.struct_size = sizeof(PgBatchBuilderConfig),
			.parent_context = test_context,
			.tuple_desc = desc,
			.ncolumns = 2,
			.capacity = 70
		});

	for (int row = 0; row < 70; row++)
	{
		MemoryContext row_oldcontext;

		row_oldcontext = MemoryContextSwitchTo(row_context);
		store_test_row(slot, row);
		MemoryContextSwitchTo(row_oldcontext);
		pg_batch_builder_append_slot(buffer, slot);
		MemoryContextReset(row_context);
	}
	batch = pg_batch_builder_finish(buffer, InvalidOid);
	if (batch == NULL || batch->selection.nrows != 70 || batch->selection.nwords != 2 ||
		batch->selection.words[0] != UINT64_MAX ||
		batch->selection.words[1] != UINT64CONST(0x3f) ||
		!pg_batch_builder_is_full(buffer))
		ok = false;
	if (ok)
	{
		batch->ops->get_datum_column(batch, 0, &batch->selection,
									PG_BATCH_COLUMN_FILTER,
									&numbers);
		batch->ops->get_datum_column(batch, 1, &batch->selection,
									PG_BATCH_COLUMN_PROJECT,
									&strings);
		if (numbers.nrows != 70 || strings.nrows != 70)
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
		uint64		all[2] = {UINT64_MAX, UINT64_MAX};
		PgBatchSelection keep_rows = {70, 2, keep};
		PgBatchSelection all_rows = {70, 2, all};

		pg_batch_selection_intersect(&batch->selection, &keep_rows);
		pg_batch_selection_intersect(&batch->selection, &all_rows);
		if (batch->selection.words[0] != keep[0] ||
			batch->selection.words[1] != keep[1])
			ok = false;
	}

	pg_batch_builder_reset(buffer);
	for (int row = 0; row < 3; row++)
	{
		store_test_row(slot, row + 100);
		pg_batch_builder_append_slot(buffer, slot);
	}
	batch = pg_batch_builder_finish(buffer, 42);
	if (batch == NULL || batch->selection.nrows != 3 || batch->selection.nwords != 1 ||
		batch->selection.words[0] != UINT64CONST(0x7) || batch->table_oid != 42)
		ok = false;

	ExecDropSingleTupleTableSlot(slot);
	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(test_context);
	PG_RETURN_BOOL(ok);
}

Datum
pg_batch_binding_request_test(PG_FUNCTION_ARGS)
{
	MemoryContext context;
	MemoryContext oldcontext;
	TupleDesc	desc;
	TupleTableSlot *slot;
	PgBatchBinding *binding;
	const PgBatchRequest *request;
	const PgBatchAPI *api = pg_batch_api_get();
	PgBatchLayout layout = PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);
	int			target_columns[2] = {0, 2};
	Bitmapset  *filters = bms_make_singleton(0);
	Bitmapset  *projects = bms_make_singleton(2);
	PgBatchRequestSpec spec = {
		.struct_size = sizeof(PgBatchRequestSpec),
		.filter_columns = filters,
		.project_columns = projects,
		.output_mode = PG_BATCH_OUTPUT_BATCH,
		.max_rows = 17
	};
	bool		ok;

	context = AllocSetContextCreate(CurrentMemoryContext,
		"pg_batch binding request test", ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(context);
	desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(desc, 1, "one", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 2, "two", INT4OID, -1, 0);
	TupleDescFinalize(desc);
	desc = BlessTupleDesc(desc);
	slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	layout.ncolumns = 3;
	layout.ntargets = 2;
	layout.target_columns = target_columns;
	binding = api->attach_slot(slot, &layout, &test_consumer_ops, NULL);
	/* The binding owns its copy of the target mapping. */
	target_columns[1] = 1;
	api->set_request(binding, &spec);
	request = api->seal_request(binding);
	ok = binding == api->find_binding(slot) &&
		request->struct_size >= PG_BATCH_REQUEST_MIN_SIZE &&
		request->layout->ncolumns == 3 && request->layout->ntargets == 2 &&
		pg_batch_layout_column(request->layout, 1) == 2 &&
		request->spec.output_mode == PG_BATCH_OUTPUT_BATCH &&
		request->spec.max_rows == 17 &&
		bms_equal(request->spec.filter_columns, filters) &&
		bms_equal(request->spec.project_columns, projects) &&
		api->get_batch(binding) == NULL;
	bms_free(filters);
	bms_free(projects);
	ExecDropSingleTupleTableSlot(slot);
	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(context);
	PG_RETURN_BOOL(ok);
}

Datum
pg_batch_request_after_seal_test(PG_FUNCTION_ARGS)
{
	TupleDesc	desc = CreateTemplateTupleDesc(1);
	TupleTableSlot *slot;
	const PgBatchAPI *api = pg_batch_api_get();
	PgBatchLayout layout = PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);
	PgBatchBinding *binding;

	TupleDescInitEntry(desc, 1, "one", INT4OID, -1, 0);
	TupleDescFinalize(desc);
	desc = BlessTupleDesc(desc);
	slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	layout.ncolumns = 1;
	layout.ntargets = 1;
	binding = api->attach_slot(slot, &layout, &test_consumer_ops, NULL);
	api->seal_request(binding);
	api->set_request(binding,
		&(PgBatchRequestSpec) {
			.struct_size = sizeof(PgBatchRequestSpec),
			.output_mode = PG_BATCH_OUTPUT_ROWS
		});
	PG_RETURN_VOID();
}

Datum
pg_batch_output_test(PG_FUNCTION_ARGS)
{
	MemoryContext context;
	MemoryContext oldcontext;
	TupleDesc	desc;
	TupleDesc	output_desc;
	TupleTableSlot *input_slot;
	TupleTableSlot *output_slot;
	PgBatchBuilder *buffer;
	PgBatchOutput *output;
	PgBatchBinding *binding;
	PgBatch *batch;
	PgBatchRowView view = PG_BATCH_STRUCT_INITIALIZER(PgBatchRowView);
	const PgBatchAPI *api = pg_batch_api_get();
	PgBatchLayout layout = PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);
	int			target_columns[2] = {1, 0};
	bool		ok = true;

	context = AllocSetContextCreate(CurrentMemoryContext,
		"pg_batch output test", ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(context);
	desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(desc, 1, "number", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 2, "label", TEXTOID, -1, 0);
	TupleDescFinalize(desc);
	desc = BlessTupleDesc(desc);
	output_desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(output_desc, 1, "label", TEXTOID, -1, 0);
	TupleDescInitEntry(output_desc, 2, "number", INT4OID, -1, 0);
	TupleDescFinalize(output_desc);
	output_desc = BlessTupleDesc(output_desc);
	input_slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	output_slot = MakeSingleTupleTableSlot(output_desc, &TTSOpsVirtual);
	buffer = pg_batch_builder_create(
		&(PgBatchBuilderConfig) {
			.struct_size = sizeof(PgBatchBuilderConfig),
			.parent_context = context,
			.tuple_desc = desc,
			.ncolumns = 2,
			.capacity = 3
		});
	for (int row = 0; row < 3; row++)
	{
		store_test_row(input_slot, row);
		pg_batch_builder_append_slot(buffer, input_slot);
	}
	batch = pg_batch_builder_finish(buffer, 42);
	layout.ncolumns = 2;
	layout.ntargets = 2;
	layout.target_columns = target_columns;
	output = pg_batch_output_create(context, output_slot, &layout);
	binding = pg_batch_output_binding(output);
	pg_batch_output_set_request(output,
		&(PgBatchRequestSpec) {
			.struct_size = sizeof(PgBatchRequestSpec),
			.output_mode = PG_BATCH_OUTPUT_ROWS,
			.max_rows = 3
		});
	if (pg_batch_output_publish(output, batch) != output_slot ||
		!output_slot->tts_isnull[0] ||
		DatumGetInt32(output_slot->tts_values[1]) != 1)
		ok = false;
	pg_batch_output_select(output, 2);
	if (output_slot->tts_isnull[0] ||
		!text_matches(output_slot->tts_values[0], 2) ||
		DatumGetInt32(output_slot->tts_values[1]) != 3)
		ok = false;
	api->get_row_view(binding, &view);
	pg_batch_row_view_select(&view, 1);
	if (output_slot->tts_isnull[0] ||
		!text_matches(output_slot->tts_values[0], 1) ||
		DatumGetInt32(output_slot->tts_values[1]) != 2)
		ok = false;
	pg_batch_output_finish(output);
	if (!api->finished(binding))
		ok = false;
	pg_batch_output_clear(output);
	if (api->get_batch(binding) != NULL || !TupIsNull(output_slot))
		ok = false;
	ExecDropSingleTupleTableSlot(output_slot);
	ExecDropSingleTupleTableSlot(input_slot);
	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(context);
	PG_RETURN_BOOL(ok);
}

Datum
pg_batch_output_over_max_test(PG_FUNCTION_ARGS)
{
	MemoryContext context = AllocSetContextCreate(CurrentMemoryContext,
		"pg_batch output maximum test", ALLOCSET_DEFAULT_SIZES);
	TupleDesc	desc = CreateTemplateTupleDesc(2);
	TupleTableSlot *input_slot;
	TupleTableSlot *output_slot;
	PgBatchBuilder *buffer;
	PgBatchOutput *output;
	PgBatchLayout layout = PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);

	TupleDescInitEntry(desc, 1, "number", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 2, "label", TEXTOID, -1, 0);
	TupleDescFinalize(desc);
	desc = BlessTupleDesc(desc);
	input_slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	output_slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	buffer = pg_batch_builder_create(
		&(PgBatchBuilderConfig) {
			.struct_size = sizeof(PgBatchBuilderConfig),
			.parent_context = context,
			.tuple_desc = desc,
			.ncolumns = 2,
			.capacity = 2
		});
	for (int row = 0; row < 2; row++)
	{
		store_test_row(input_slot, row);
		pg_batch_builder_append_slot(buffer, input_slot);
	}
	layout.ncolumns = 2;
	layout.ntargets = 2;
	output = pg_batch_output_create(context, output_slot, &layout);
	pg_batch_output_set_request(output,
		&(PgBatchRequestSpec) {
			.struct_size = sizeof(PgBatchRequestSpec),
			.output_mode = PG_BATCH_OUTPUT_BATCH,
			.max_rows = 1
		});
	pg_batch_output_publish(output,
		pg_batch_builder_finish(buffer, InvalidOid));
	PG_RETURN_VOID();
}

static PgBatchOutput *
make_active_output(MemoryContext context)
{
	TupleDesc	desc;
	TupleTableSlot *input_slot;
	TupleTableSlot *output_slot;
	PgBatchBuilder *buffer;
	PgBatchOutput *output;
	PgBatch *batch;
	PgBatchLayout layout = PG_BATCH_STRUCT_INITIALIZER(PgBatchLayout);

	desc = CreateTemplateTupleDesc(2);
	TupleDescInitEntry(desc, 1, "number", INT4OID, -1, 0);
	TupleDescInitEntry(desc, 2, "label", TEXTOID, -1, 0);
	TupleDescFinalize(desc);
	desc = BlessTupleDesc(desc);
	input_slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	output_slot = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	buffer = pg_batch_builder_create(
		&(PgBatchBuilderConfig) {
			.struct_size = sizeof(PgBatchBuilderConfig),
			.parent_context = context,
			.tuple_desc = desc,
			.ncolumns = 2,
			.capacity = 1
		});
	store_test_row(input_slot, 0);
	pg_batch_builder_append_slot(buffer, input_slot);
	batch = pg_batch_builder_finish(buffer, InvalidOid);
	layout.ncolumns = 2;
	layout.ntargets = 2;
	output = pg_batch_output_create(context, output_slot, &layout);
	pg_batch_output_publish(output, batch);
	return output;
}

static PgBatchOutput *
make_finished_output(MemoryContext context)
{
	PgBatchOutput *output = make_active_output(context);

	pg_batch_output_finish(output);
	return output;
}

Datum
pg_batch_output_double_finish_test(PG_FUNCTION_ARGS)
{
	MemoryContext context = AllocSetContextCreate(CurrentMemoryContext,
		"pg_batch double finish test", ALLOCSET_DEFAULT_SIZES);
	PgBatchOutput *output = make_finished_output(context);

	pg_batch_output_finish(output);
	PG_RETURN_VOID();
}

Datum
pg_batch_output_select_after_finish_test(PG_FUNCTION_ARGS)
{
	MemoryContext context = AllocSetContextCreate(CurrentMemoryContext,
		"pg_batch select after finish test", ALLOCSET_DEFAULT_SIZES);
	PgBatchOutput *output = make_finished_output(context);

	pg_batch_output_select(output, 0);
	PG_RETURN_VOID();
}

Datum
pg_batch_output_fast_select_after_finish_test(PG_FUNCTION_ARGS)
{
	const PgBatchAPI *api = pg_batch_api_get();
	MemoryContext context = AllocSetContextCreate(CurrentMemoryContext,
		"pg_batch fast select after finish test", ALLOCSET_DEFAULT_SIZES);
	PgBatchOutput *output = make_active_output(context);
	PgBatchRowView view = PG_BATCH_STRUCT_INITIALIZER(PgBatchRowView);

	api->get_row_view(pg_batch_output_binding(output), &view);
	pg_batch_output_finish(output);
	pg_batch_row_view_select(&view, 0);
	PG_RETURN_VOID();
}
