/*
 * Type-checked storage for CustomPath and CustomScan private plan data.
 *
 * PostgreSQL requires custom_private to be a copyObject()-compatible List.
 * These helpers keep that representation private and identify fields by name
 * so planner and executor code do not share untyped list positions.
 */
#ifndef PG_BATCH_PLAN_H
#define PG_BATCH_PLAN_H

#include "postgres.h"

#include "nodes/bitmapset.h"
#include "nodes/pg_list.h"

typedef struct PgBatchPlanWriter PgBatchPlanWriter;
typedef struct PgBatchPlanReader PgBatchPlanReader;

extern PgBatchPlanWriter *pg_batch_plan_writer_create(const char *kind,
	int version);
extern void pg_batch_plan_write_int(PgBatchPlanWriter *writer,
	const char *name, int value);
extern void pg_batch_plan_write_string(PgBatchPlanWriter *writer,
	const char *name, const char *value);
extern void pg_batch_plan_write_node(PgBatchPlanWriter *writer,
	const char *name, const Node *value);
extern void pg_batch_plan_write_list(PgBatchPlanWriter *writer,
	const char *name, const List *value);
extern void pg_batch_plan_write_int_list(PgBatchPlanWriter *writer,
	const char *name, const List *value);
extern void pg_batch_plan_write_bitmap(PgBatchPlanWriter *writer,
	const char *name, const Bitmapset *value);
extern List *pg_batch_plan_writer_finish(PgBatchPlanWriter *writer);

extern PgBatchPlanReader *pg_batch_plan_reader_create(const List *data,
	const char *kind, int version);
extern int pg_batch_plan_read_int(PgBatchPlanReader *reader,
	const char *name);
extern const char *pg_batch_plan_read_string(PgBatchPlanReader *reader,
	const char *name);
extern Node *pg_batch_plan_read_node(PgBatchPlanReader *reader,
	const char *name);
extern List *pg_batch_plan_read_list(PgBatchPlanReader *reader,
	const char *name);
extern List *pg_batch_plan_read_int_list(PgBatchPlanReader *reader,
	const char *name);
extern Bitmapset *pg_batch_plan_read_bitmap(PgBatchPlanReader *reader,
	const char *name);
extern void pg_batch_plan_reader_finish(PgBatchPlanReader *reader);

#endif /* PG_BATCH_PLAN_H */
