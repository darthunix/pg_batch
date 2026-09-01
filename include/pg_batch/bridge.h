/*
 * Common ABI for passing batches between independently built PostgreSQL
 * extensions.
 *
 * A consumer attaches a batch request to a TupleTableSlot. A registered
 * source can inspect that request at a scan boundary and publish a batch on
 * the same slot. The bridge owns the attachment and batch lifecycle, but does
 * not interpret expressions or source-native column formats.
 *
 * Unless stated otherwise, pointers passed through this interface are
 * borrowed and remain owned by the component that supplied them.
 */
#ifndef PG_BATCH_BRIDGE_H
#define PG_BATCH_BRIDGE_H

#include "postgres.h"

#include "batch.h"
#include "layout.h"
#include "nodes/pathnodes.h"
#include "utils/rel.h"

/*
 * Change both values when an incompatible layout or calling convention is
 * introduced. Extensible operation tables and published batches carry the
 * ABI version and their actual size so the bridge can reject incompatible
 * objects before calling through them.
 */
#define PG_BATCH_RENDEZVOUS "pg_batch_api_v1"
#define PG_BATCH_API_ABI_VERSION 1
#define PG_BATCH_API_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchAPI, detach_binding)

/*
 * Opaque bridge-owned association between a TupleTableSlot, its batch
 * request, and its active batch. It is owned by the slot's memory context.
 */
typedef struct PgBatchBinding PgBatchBinding;
typedef struct PgBatchNodeOps PgBatchNodeOps;
typedef struct PgBatchSourceOps PgBatchSourceOps;
struct PlanState;

/* Representation expected by the parent of a batch-aware node. */
typedef enum PgBatchOutputMode
{
	PG_BATCH_OUTPUT_ROWS,
	PG_BATCH_OUTPUT_BATCH
} PgBatchOutputMode;

/* Mutable request fields copied by the bridge when a binding is configured. */
typedef struct PgBatchRequestSpec
{
	Size		struct_size;
	const Bitmapset *filter_columns;
	const Bitmapset *project_columns;
	PgBatchOutputMode output_mode;
	/* Maximum rows accepted at once, or zero for no explicit limit. */
	int			max_rows;
} PgBatchRequestSpec;

#define PG_BATCH_REQUEST_SPEC_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchRequestSpec, max_rows)

/*
 * Runtime request attached to one batch-aware TupleTableSlot.
 *
 * layout describes logical columns shared by plan nodes. The masks in spec
 * use these compact column numbers; projection columns are needed only for
 * rows that survive filtering. spec.max_rows is zero when the consumer has no
 * batch-size limit.
 */
typedef struct PgBatchRequest
{
	/* Actual size owned by the bridge; new fields are appended at the end. */
	Size		struct_size;
	const PgBatchLayout *layout;
	PgBatchRequestSpec spec;
}			PgBatchRequest;

#define PG_BATCH_REQUEST_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchRequest, spec)

/*
 * Bridge entry points published through PG_BATCH_RENDEZVOUS. The table
 * is owned by the bridge and remains valid for the lifetime of the backend.
 */
typedef struct PgBatchAPI
{
	/* Set to PG_BATCH_API_ABI_VERSION and sizeof(this structure). */
	uint32		abi_version;
	Size		struct_size;

	/* Register a source table; the bridge borrows it until unregistration. */
	void		(*register_source) (const PgBatchSourceOps *source);
	/* Remove the source with this name after all of its scans have ended. */
	void		(*unregister_source) (const char *source_name);
	/* Find the sole source supporting relation, or NULL; duplicates error. */
	const PgBatchSourceOps *(*find_source) (Relation relation);
	/* Find a registered source by its exact stable name, or return NULL. */
	const PgBatchSourceOps *(*get_source) (const char *source_name);

	/* Register a batch-producing plan-node implementation. */
	void		(*register_node) (const PgBatchNodeOps *node);
	/* Remove a node after all plans using it have finished. */
	void		(*unregister_node) (const char *node_name);
	/* Find the sole registered node claiming path, or return NULL. */
	const PgBatchNodeOps *(*find_node) (const Path *path);
	/* Find a node by the stable name stored in a parent plan. */
	const PgBatchNodeOps *(*get_node) (const char *node_name);

	/*
	 * Find an existing slot binding without creating one. This lookup is meant
	 * for extension boundaries that receive only TupleTableSlot *, such as a
	 * table access method callback. Batch-aware executors should retain the
	 * returned binding instead of repeating the lookup.
	 */
	PgBatchBinding *(*find_binding) (TupleTableSlot *slot);
	/*
	 * Attach a new binding to slot and return it. The bridge copies layout into
	 * the slot's memory context and borrows consumer_ops. Attaching the same
	 * slot twice is an error.
	 */
	PgBatchBinding *(*attach_slot) (TupleTableSlot *slot,
									const PgBatchLayout *layout,
									const PgBatchConsumerOps *consumer_ops,
									void *consumer_state);
	/* Replace the request before it is sealed; pointed-to masks are copied. */
	void		(*set_request) (PgBatchBinding *binding,
								 const PgBatchRequestSpec *spec);
	/* Store borrowed source identity and execution state on the binding. */
	void		(*set_source) (PgBatchBinding *binding,
								  const char *source_name,
								  void *source_state);
	/* Return the bound source state and optionally its stable name. */
	void	   *(*get_bound_source) (PgBatchBinding *binding,
									 const char **source_name);
	/* Freeze the request and return its borrowed read-only representation. */
	const PgBatchRequest *(*seal_request) (PgBatchBinding *binding);
	/* Return the binding's borrowed, read-only runtime request, or NULL. */
	const PgBatchRequest *(*get_request) (PgBatchBinding *binding);

	/*
	 * Install one source-owned batch and notify the slot consumer. The binding
	 * must not already have an active batch. The bridge validates the common
	 * batch and operation layouts before publishing them.
	 */
	void		(*publish_batch) (PgBatchBinding *binding, PgBatch *batch);
	/* Return the binding's active borrowed batch, or NULL. */
	PgBatch    *(*get_batch) (PgBatchBinding *binding);
	/* Expose one selected batch row through the binding's scalar slot view. */
	void		(*select_row) (PgBatchBinding *binding, int row);
	/* Mark the active batch consumed so a batch-aware parent may request more. */
	void		(*finish) (PgBatchBinding *binding);
	/* Return true when no unfinished batch remains on this binding. */
	bool		(*finished) (PgBatchBinding *binding);
	/* Remove and release the active batch while retaining the slot binding. */
	void		(*clear) (PgBatchBinding *binding);
	/* Release any batch, remove the slot attachment, and free the binding. */
	void		(*detach_binding) (PgBatchBinding *binding);

	/*
	 * Optionally return a fast row-selection view. The caller initializes
	 * struct_size; the binding and its consumer own the returned pointers.
	 */
	void		(*get_row_view) (PgBatchBinding *binding,
							 PgBatchRowView *result);
}			PgBatchAPI;

#endif							/* PG_BATCH_BRIDGE_H */
