#ifndef PG_BATCH_LIMIT_H
#define PG_BATCH_LIMIT_H

#include "postgres.h"

#include "nodes/extensible.h"
#include "bridge.h"
#include "node.h"
#include "plan.h"
#include "runtime.h"

#define PG_BATCH_LIMIT_NODE_NAME "pg_batch_limit"

extern bool pg_batch_limit_enable;
extern const PgBatchAPI *pg_batch_limit_api;
extern const PgBatchNodeOps pg_batch_limit_node_ops;

typedef struct PgBatchLimitPlanData
{
	const char *node_name;
	List	   *output_columns;
	int			column_count;
} PgBatchLimitPlanData;

extern void pg_batch_limit_planner_init(void);
extern void pg_batch_limit_planner_fini(void);
extern Node *pg_batch_limit_create_state(CustomScan *scan);
extern void pg_batch_limit_read_plan(const CustomScan *scan,
	PgBatchLimitPlanData *result);
extern PgBatchBinding *pg_batch_limit_request_binding(
	struct PlanState *planstate);

#endif /* PG_BATCH_LIMIT_H */
