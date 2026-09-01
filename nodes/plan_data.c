#include "postgres.h"

#include "internal.h"

static PgBatchPlanReader *
plan_reader(const CustomScan *scan, const char *kind)
{
	if (scan == NULL)
		elog(ERROR, "pg_batch received a null custom plan");
	return pg_batch_plan_reader_create(scan->custom_private, kind,
		PG_BATCH_PLAN_DATA_VERSION);
}

void
pg_batch_read_scan_plan(const CustomScan *scan, PgBatchScanPlanData *result)
{
	PgBatchPlanReader *reader = plan_reader(scan, "scan");

	result->source_attnums = pg_batch_plan_read_int_list(reader,
		"source_attnums");
	result->nfilter_columns = pg_batch_plan_read_int(reader,
		"nfilter_columns");
	result->heap_scan_mode = pg_batch_plan_read_int(reader, "heap_scan_mode");
	result->source_name = pg_batch_plan_read_string(reader, "source_name");
	result->source_private = pg_batch_plan_read_node(reader, "source_private");
	result->nsource_exprs = pg_batch_plan_read_int(reader, "nsource_exprs");
	result->batch_recheck_flags = pg_batch_plan_read_int_list(reader,
		"batch_recheck_flags");
	result->exact_filter_columns = pg_batch_plan_read_int_list(reader,
		"exact_filter_columns");
	pg_batch_plan_reader_finish(reader);
}

void
pg_batch_read_filter_plan(const CustomScan *scan,
						  PgBatchFilterPlanData *result)
{
	PgBatchPlanReader *reader = plan_reader(scan, "filter");

	result->source_attnums = pg_batch_plan_read_int_list(reader,
		"source_attnums");
	result->nfilter_columns = pg_batch_plan_read_int(reader,
		"nfilter_columns");
	result->project_columns = pg_batch_plan_read_int_list(reader,
		"project_columns");
	pg_batch_plan_reader_finish(reader);
}

void
pg_batch_read_project_plan(const CustomScan *scan,
						   PgBatchProjectPlanData *result)
{
	PgBatchPlanReader *reader = plan_reader(scan, "project");

	result->child_name = pg_batch_plan_read_string(reader, "child_name");
	result->input_columns = pg_batch_plan_read_int_list(reader,
		"input_columns");
	pg_batch_plan_reader_finish(reader);
}

void
pg_batch_read_hash_plan(const CustomScan *scan, PgBatchHashPlanData *result)
{
	PgBatchPlanReader *reader = plan_reader(scan, "hash_join");

	result->outer_columns = pg_batch_plan_read_int_list(reader,
		"outer_columns");
	result->inner_columns = pg_batch_plan_read_int_list(reader,
		"inner_columns");
	result->outer_keys = pg_batch_plan_read_int_list(reader, "outer_keys");
	result->inner_keys = pg_batch_plan_read_int_list(reader, "inner_keys");
	result->planned_partitions = pg_batch_plan_read_int(reader,
		"planned_partitions");
	result->outer_name = pg_batch_plan_read_string(reader, "outer_name");
	result->inner_name = pg_batch_plan_read_string(reader, "inner_name");
	pg_batch_plan_reader_finish(reader);
}

void
pg_batch_read_agg_plan(const CustomScan *scan, PgBatchAggPlanData *result)
{
	PgBatchPlanReader *reader = plan_reader(scan, "aggregate");

	result->child_name = pg_batch_plan_read_string(reader, "child_name");
	result->kinds = pg_batch_plan_read_int_list(reader, "kinds");
	result->columns = pg_batch_plan_read_int_list(reader, "columns");
	if (list_length(result->kinds) != list_length(result->columns))
		elog(ERROR, "pg_batch aggregate plan fields are misaligned");
	pg_batch_plan_reader_finish(reader);
}
