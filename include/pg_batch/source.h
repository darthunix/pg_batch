/*
 * Planning and execution contract for batch-producing scan sources.
 *
 * A source can be a table access method, FDW, or another extension-owned
 * scanner. It classifies restrictions, receives separate filter and lazy
 * projection columns, and returns batches through the common bridge.
 */
#ifndef PG_BATCH_SOURCE_H
#define PG_BATCH_SOURCE_H

#include "postgres.h"

#include "commands/explain.h"
#include "bridge.h"
#include "nodes/plannodes.h"

/* How completely a source can handle one scan restriction. */
typedef enum PgBatchQualSupport
{
	/* The source cannot use the restriction. */
	PG_BATCH_QUAL_UNSUPPORTED,
	/* The source may prune storage, but PostgreSQL must recheck every row. */
	PG_BATCH_QUAL_PRUNE_ONLY,
	/* The source applies the restriction exactly; no recheck is required. */
	PG_BATCH_QUAL_EXACT
} PgBatchQualSupport;

/*
 * Borrowed planner input passed to one source's plan_scan() callback.
 *
 * clauses contains bare relation restrictions in executor order.
 * restrictinfos contains the matching RestrictInfo nodes in the same order so
 * a source can preserve PostgreSQL's security ordering. Attribute bitmaps use
 * one-based physical relation numbers. Projection attributes already used by
 * filters are omitted from project_attnums.
 */
typedef struct PgBatchSourcePlanRequest
{
	Size		struct_size;
	PlannerInfo *root;
	RelOptInfo *rel;
	Relation	relation;
	List	   *clauses;
	List	   *restrictinfos;
	const Bitmapset *filter_attnums;
	const Bitmapset *project_attnums;
} PgBatchSourcePlanRequest;

#define PG_BATCH_SOURCE_PLAN_REQUEST_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchSourcePlanRequest, project_attnums)

/* Copyable source plan returned from plan_scan() in the planner context. */
typedef struct PgBatchSourcePlanResult
{
	/* Set by the caller. The provider fills fields without clearing it. */
	Size		struct_size;
	/* One classification for every input clause, in the same order. */
	int			nquals;
	PgBatchQualSupport *qual_support;
	/* Expressions PostgreSQL must process with setrefs before execution. */
	List	   *source_exprs;
	/* Source-owned copyObject()-compatible payload saved in the CustomScan. */
	Node	   *source_private;
} PgBatchSourcePlanResult;

#define PG_BATCH_SOURCE_PLAN_RESULT_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchSourcePlanResult, source_private)

/* Borrowed input passed once to the selected source's begin_scan(). */
typedef struct PgBatchSourceExecRequest
{
	Size		struct_size;
	Relation	relation;
	struct PlanState *parent;
	Node	   *source_private;
	List	   *source_exprs;
	MemoryContext query_context;
	const PgBatchRequest *slot_request;
	/* Source-specific mapping from compact columns to physical attributes. */
	const AttrNumber *source_attnums;
	int			nsource_columns;
} PgBatchSourceExecRequest;

#define PG_BATCH_SOURCE_EXEC_REQUEST_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchSourceExecRequest, nsource_columns)

/*
 * Operations implemented by an independently built batch source.
 *
 * The table has a unique stable name and remains valid while registered. A
 * scan calls plan_scan(), begin_scan(), optional rescans, and finally
 * end_scan().
 */
struct PgBatchProviderOps
{
	uint32		abi_version;
	Size		struct_size;
	const char *provider_name;
	/* Return whether this source can scan the open relation. */
	bool		(*supports_relation) (Relation relation);
	/* Classify all restrictions and return source-owned plan data. */
	void		(*plan_scan) (const PgBatchSourcePlanRequest *request,
								  PgBatchSourcePlanResult *result);
	/* Start a scan and return opaque state for the remaining callbacks. */
	void	   *(*begin_scan) (const PgBatchSourceExecRequest *request);
	/* Required lifecycle operations. */
	void		(*rescan) (void *provider_state);
	void		(*end_scan) (void *provider_state);
	/* Optional append-only operations. */
	/*
	 * Return the next source-owned batch, or NULL at end of scan. A NULL
	 * callback keeps the table-AM path, where the source publishes through the
	 * scan slot instead.
	 */
	PgBatch *(*next_batch) (void *provider_state);
	/* Add source-specific EXPLAIN properties; this callback is optional. */
	void		(*explain) (void *provider_state, ExplainState *es);
};

#define PG_BATCH_PROVIDER_OPS_ABI_VERSION 1
#define PG_BATCH_PROVIDER_OPS_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchProviderOps, end_scan)

#endif /* PG_BATCH_SOURCE_H */
