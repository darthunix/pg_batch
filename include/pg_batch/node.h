/* Contract for passing batches between independently built plan nodes. */
#ifndef PG_BATCH_NODE_H
#define PG_BATCH_NODE_H

#include "postgres.h"

#include "bridge.h"
#include "nodes/plannodes.h"

/* Stable mapping from a Plan target list to compact batch columns. */
typedef struct PgBatchOutputLayout
{
	/* Set by the caller. The producer fills fields without clearing it. */
	Size		struct_size;
	int			ncolumns;
	/* One zero-based batch column for every target entry in the Plan. */
	const int  *batch_columns;
} PgBatchOutputLayout;

#define PG_BATCH_OUTPUT_LAYOUT_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchOutputLayout, batch_columns)

/*
 * Operations implemented by an extension whose plan nodes publish batches.
 *
 * When return_batch is requested, every nonempty ExecProcNode() result must
 * be a bridge-attached slot with an active batch. Pass-through nodes may
 * return a child slot instead of the slot that holds their request binding.
 */
struct PgBatchProducerOps
{
	uint32		abi_version;
	Size		struct_size;
	/* Stable name stored in parent plans and used again during execution. */
	const char *producer_name;
	/* Return true when this Path can publish batches through the bridge. */
	bool		(*supports_path) (const Path *path);
	/* Describe how Plan target entries map to compact batch columns. */
	void		(*get_output_layout) (const Plan *plan,
								  PgBatchOutputLayout *result);
	/* Return the binding through which a parent configures this node. */
	PgBatchBinding *(*get_request_binding) (struct PlanState *planstate);
};

#define PG_BATCH_PRODUCER_OPS_ABI_VERSION 1
#define PG_BATCH_PRODUCER_OPS_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchProducerOps, get_request_binding)

#endif /* PG_BATCH_NODE_H */
