#ifndef PG_BATCH_FDW_H
#define PG_BATCH_FDW_H

#include "postgres.h"

#include "commands/explain.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "nodes/pathnodes.h"
#include "utils/rel.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
#include "nanoarrow/nanoarrow.h"
#include "nanoarrow/nanoarrow_ipc.h"
#pragma GCC diagnostic pop

#include "bridge.h"
#include "source.h"
#include "arrow.h"
#include "kernels.h"
#include "runtime.h"
#include "expr.h"

typedef struct ArrowArray ArrowArray;
typedef struct ArrowArrayView ArrowArrayView;
typedef struct ArrowBuffer ArrowBuffer;
typedef struct ArrowBufferView ArrowBufferView;
typedef struct ArrowError ArrowError;
typedef struct ArrowIpcDecoder ArrowIpcDecoder;
typedef struct ArrowIpcOutputStream ArrowIpcOutputStream;
typedef struct ArrowIpcWriter ArrowIpcWriter;
typedef struct ArrowSchema ArrowSchema;

#define PG_BATCH_FDW_PROVIDER_NAME "pg_batch_arrow_fdw"
#define PG_BATCH_FDW_BATCH_SIZE 64

typedef struct PgBatchFdwStats
{
	uint64		record_batches;
	uint64		batch_windows;
	uint64		rows_removed;
	uint64		bytes_read;
	uint64		columns_decoded;
	uint64		filter_columns_decoded;
	uint64		project_columns_decoded;
	uint64		filter_datums;
	uint64		project_datums;
} PgBatchFdwStats;

typedef struct PgBatchFdwScan PgBatchFdwScan;

extern bool pg_batch_fdw_pushdown;
extern bool pg_batch_fdw_column_pruning;
extern const PgBatchAPI *pg_batch_fdw_api;
extern const PgBatchProviderOps pg_batch_fdw_provider_ops;

extern FdwRoutine *pg_batch_fdw_routine(void);
extern char *pg_batch_fdw_get_filename(Oid relation_oid);
extern void pg_batch_fdw_validate_relation(Relation relation);

extern PgBatchFdwScan *pg_batch_fdw_scan_begin(
	Relation relation, PlanState *parent, Node *source_private,
	List *source_exprs, const PgBatchRequest *request,
	MemoryContext query_context);
extern PgBatch *pg_batch_fdw_scan_next(PgBatchFdwScan *scan);
extern void pg_batch_fdw_scan_rescan(PgBatchFdwScan *scan);
extern void pg_batch_fdw_scan_end(PgBatchFdwScan *scan);
extern void pg_batch_fdw_scan_explain(PgBatchFdwScan *scan, ExplainState *es);

extern void pg_batch_fdw_begin_foreign_scan(ForeignScanState *node, int eflags);
extern TupleTableSlot *pg_batch_fdw_iterate_foreign_scan(ForeignScanState *node);
extern void pg_batch_fdw_rescan_foreign_scan(ForeignScanState *node);
extern void pg_batch_fdw_end_foreign_scan(ForeignScanState *node);
extern void pg_batch_fdw_explain_foreign_scan(ForeignScanState *node,
											   ExplainState *es);

#endif							/* PG_BATCH_FDW_H */
