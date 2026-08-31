#ifndef PG_BATCH_LIMIT_H
#define PG_BATCH_LIMIT_H

#include "postgres.h"

#include "nodes/extensible.h"
#include "bridge.h"
#include "node.h"
#include "runtime.h"

#define PG_BATCH_LIMIT_PRODUCER_NAME "pg_batch_limit"

extern bool pg_batch_limit_enable;
extern const PgBatchAPI *pg_batch_limit_api;
extern const PgBatchProducerOps pg_batch_limit_producer_ops;

extern void pg_batch_limit_planner_init(void);
extern void pg_batch_limit_planner_fini(void);
extern Node *pg_batch_limit_create_state(CustomScan *scan);
extern PgBatchBinding *pg_batch_limit_request_binding(
	struct PlanState *planstate);

#endif /* PG_BATCH_LIMIT_H */
