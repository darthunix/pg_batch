#ifndef PG_BATCH_TAM_H
#define PG_BATCH_TAM_H

#include "executor/executor.h"
#include "nodes/primnodes.h"
#include "utils/rel.h"

#include "bridge.h"
#include "arrow.h"

#define PG_BATCH_TAM_PROVIDER_NAME "pg_batch_compressed_tam"
#define PG_BATCH_SIZE 64
#define PG_BATCH_ALL_ROWS UINT64_MAX

typedef enum SourceScanMode
{
	PG_BATCH_TAM_BATCH,
	PG_BATCH_TAM_PRUNE,
	PG_BATCH_TAM_FILTER
} SourceScanMode;

typedef enum SourceOperator
{
	PG_BATCH_TAM_SOURCE_EQ,
	PG_BATCH_TAM_SOURCE_NE,
	PG_BATCH_TAM_SOURCE_LT,
	PG_BATCH_TAM_SOURCE_LE,
	PG_BATCH_TAM_SOURCE_GT,
	PG_BATCH_TAM_SOURCE_GE
} SourceOperator;

typedef struct SourceQual
{
	AttrNumber	attnum;
	SourceOperator op;
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
	const PgBatchBridgeRequest *request;
	ExprContext *econtext;
	SourceQual *quals;
	int			nquals;
	SourceScanMode mode;
	int			group_index;
	int			batch_index;
	bool		group_ready;
	bool		quals_ready;
	bool		relation_acquired;
	ActiveBatch *active;
	CompressedStats stats;
} CompressedScan;

extern bool pg_batch_tam_enable;
extern int	pg_batch_tam_scan_mode;
extern const PgBatchBridgeAPI *pg_batch_tam_bridge;
extern const PgBatchBridgeProviderOps pg_batch_tam_provider_ops;

extern bool pg_batch_relation_uses_tableam(Relation relation);
extern bool pg_batch_compressed_available(Relation relation);
extern CompressedRelation *pg_batch_compressed_acquire(Relation relation);
extern void pg_batch_compressed_release(CompressedRelation *relation);
extern bool pg_batch_compressed_scan_next(PgBatchBridgeBinding *binding,
										  CompressedScan *scan);
extern void pg_batch_compressed_fini(void);

#endif							/* PG_BATCH_TAM_H */
