/* Contract for passing batches between independently built plan nodes. */
#ifndef PG_BATCH_NODE_H
#define PG_BATCH_NODE_H

#include "postgres.h"

#include "bridge.h"
#include "nodes/plannodes.h"

/*
 * Operations implemented by an extension whose plan nodes publish batches.
 *
 * With PG_BATCH_OUTPUT_BATCH, every nonempty ExecProcNode() result must be a
 * bridge-attached slot with an active batch. Forwarding nodes may return a
 * child slot instead of the slot that holds their request binding.
 */
struct PgBatchNodeOps
{
	uint32		abi_version;
	Size		struct_size;
	/* Stable name stored in parent plans and used again during execution. */
	const char *node_name;
	/* Return true when this Path can publish batches through the bridge. */
	bool		(*supports_path) (const Path *path);
	/* Describe how Plan target entries map to compact batch columns. */
	void		(*get_layout) (const Plan *plan, PgBatchLayout *result);
	/* Return the binding through which a parent configures this node. */
	PgBatchBinding *(*get_request_binding) (struct PlanState *planstate);
};

#define PG_BATCH_NODE_OPS_ABI_VERSION 1
#define PG_BATCH_NODE_OPS_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchNodeOps, get_request_binding)

#endif /* PG_BATCH_NODE_H */
