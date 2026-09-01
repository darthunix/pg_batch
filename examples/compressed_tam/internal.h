#ifndef PG_BATCH_TAM_H
#define PG_BATCH_TAM_H

#include "executor/executor.h"
#include "nodes/primnodes.h"
#include "utils/rel.h"

#include "bridge.h"
#include "source.h"
#include "arrow.h"
#include "kernels.h"
#include "plan.h"
#include "runtime.h"
#include "expr.h"

#define PG_BATCH_TAM_SOURCE_NAME "pg_batch_compressed_tam"
#define PG_BATCH_SIZE 64
#define PG_BATCH_ALL_ROWS UINT64_MAX

typedef enum SourceScanMode
{
	PG_BATCH_TAM_BATCH,
	PG_BATCH_TAM_PRUNE,
	PG_BATCH_TAM_FILTER
} SourceScanMode;

typedef struct SourceQual
{
	PgBatchExpr *expr;
	Bitmapset  *column_mask;
	int			column;
	bool		prunable;
	AttrNumber	attnum;
	PgBatchInt4Op op;
	ExprState  *scalar_expr;
	NullableDatum scalar;
} SourceQual;

typedef struct CompressedStats
{
	uint64		groups_examined;
	uint64		groups_skipped_minmax;
	uint64		groups_skipped_membership;
	uint64		rows_removed_by_source_filter;
	uint64		encoded_bytes_touched;
	uint64		arrow_filter_columns;
	uint64		arrow_project_columns;
	uint64		arrow_decoded_values;
	uint64		filter_datums;
	uint64		project_datums;
} CompressedStats;

typedef struct CompressedRelation CompressedRelation;
typedef struct ActiveBatch ActiveBatch;

typedef struct CompressedScan
{
	MemoryContextCallback cleanup;
	MemoryContext context;
	MemoryContext batch_context;
	CompressedRelation *relation;
	const PgBatchRequest *request;
	const AttrNumber *source_attnums;
	int			ncolumns;
	ExprContext *econtext;
	SourceQual *quals;
	int			nquals;
	SourceScanMode mode;
	int			group_index;
	int			batch_index;
	int			batch_row;
	bool		group_ready;
	bool		prune_quals_ready;
	bool		relation_acquired;
	ActiveBatch *active;
	CompressedStats stats;
} CompressedScan;

extern bool pg_batch_tam_enable;
extern int	pg_batch_tam_scan_mode;
extern const PgBatchAPI *pg_batch_tam_api;
extern const PgBatchSourceOps pg_batch_tam_source_ops;

extern bool pg_batch_relation_uses_tableam(Relation relation);
extern bool pg_batch_compressed_available(Relation relation);
extern CompressedRelation *pg_batch_compressed_acquire(Relation relation);
extern void pg_batch_compressed_release(CompressedRelation *relation);
extern bool pg_batch_compressed_scan_next(PgBatchBinding *binding,
										  CompressedScan *scan);
extern void pg_batch_compressed_fini(void);

#endif							/* PG_BATCH_TAM_H */
