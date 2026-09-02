/* Helpers for building batch-aware CustomPath and CustomScan nodes. */
#ifndef PG_BATCH_PLANNER_H
#define PG_BATCH_PLANNER_H

#include "postgres.h"

#include "bridge.h"
#include "layout.h"
#include "node.h"
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"

/* How a plan node describes the columns in the batches it publishes. */
typedef enum PgBatchLayoutPolicy
{
	/* Preserve one child's layout when this node keeps its target list. */
	PG_BATCH_LAYOUT_PRESERVE_CHILD,
	/* Follow the final target list, including a late projection replacement. */
	PG_BATCH_LAYOUT_DENSE,
	/* Publish the caller-supplied layout. */
	PG_BATCH_LAYOUT_EXPLICIT
} PgBatchLayoutPolicy;

/* Complete description of a CustomPath to create from an existing Path. */
typedef struct PgBatchPathConfig
{
	Size		struct_size;
	/* All generic planner properties are copied from this path. */
	const Path *template_path;
	const CustomPathMethods *methods;
	List	   *children;
	List	   *restrictinfo;
	/* Expressions and private data carried safely to PlanCustomPath. */
	const List *expressions;
	const Node *node_data;
	uint32		flags;
} PgBatchPathConfig;

#define PG_BATCH_PATH_CONFIG_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchPathConfig, flags)

/* Decoded provider-owned data carried by a PgBatchPathConfig. */
typedef struct PgBatchPathInfo
{
	Size		struct_size;
	List	   *expressions;
	Node	   *node_data;
} PgBatchPathInfo;

#define PG_BATCH_PATH_INFO_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchPathInfo, node_data)

/* One child visible while PlanCustomPath builds provider-specific data. */
typedef struct PgBatchPlanChild
{
	Size		struct_size;
	Path	   *path;
	Plan	   *plan;
	/* NULL means that this child only provides ordinary rows. */
	const PgBatchNodeOps *node;
	/* Valid only when node is not NULL. */
	PgBatchLayout layout;
} PgBatchPlanChild;

#define PG_BATCH_PLAN_CHILD_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchPlanChild, layout)

/* Complete description of the CustomScan returned by PlanCustomPath. */
typedef struct PgBatchPlanConfig
{
	Size		struct_size;
	const PgBatchAPI *api;
	const CustomScanMethods *methods;
	PgBatchLayoutPolicy layout_policy;
	/* Child to preserve, used only by PG_BATCH_LAYOUT_PRESERVE_CHILD. */
	int			layout_child;
	/* Used only by PG_BATCH_LAYOUT_EXPLICIT. */
	const PgBatchLayout *explicit_layout;
	/* These are final executor expressions; no clauses are added implicitly. */
	const List *qual;
	const List *expressions;
	const Node *node_data;
	/* NULL derives the scan tuple list from the layout policy. */
	const List *scan_targetlist;
	Index		scanrelid;
} PgBatchPlanConfig;

#define PG_BATCH_PLAN_CONFIG_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchPlanConfig, scanrelid)

/* Common plan information read during executor initialization. */
typedef struct PgBatchPlanInfo
{
	Size		struct_size;
	int			nchildren;
	/* NULL entries identify ordinary row-producing children. */
	const char **child_names;
	PgBatchLayout layout;
	Node	   *node_data;
} PgBatchPlanInfo;

#define PG_BATCH_PLAN_INFO_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchPlanInfo, node_data)

extern CustomPath *pg_batch_path_create(const PgBatchPathConfig *config);
extern bool pg_batch_path_matches(const Path *path,
	const CustomPathMethods *methods);
extern void pg_batch_path_get_info(const CustomPath *path,
	PgBatchPathInfo *result);

/* Return true when the requested child publishes batches. */
extern bool pg_batch_plan_child(const PgBatchAPI *api,
	const CustomPath *path, const List *child_plans, int child_index,
	PgBatchPlanChild *result);

extern Plan *pg_batch_plan_create(RelOptInfo *rel, CustomPath *path,
	List *targetlist, List *child_plans, const PgBatchPlanConfig *config);
extern void pg_batch_plan_get_info(const CustomScan *scan,
	PgBatchPlanInfo *result);

/* Standard PgBatchNodeOps.get_layout callback for plans made above. */
extern void pg_batch_plan_get_layout(const Plan *plan,
	PgBatchLayout *result);

#endif /* PG_BATCH_PLANNER_H */
