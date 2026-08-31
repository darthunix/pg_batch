/*
 * Common ABI for passing batches between independently built PostgreSQL
 * extensions.
 *
 * A consumer attaches a batch request to a TupleTableSlot. A registered
 * provider can inspect that request at a scan boundary and publish a batch on
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
typedef struct PgBatchProducerOps PgBatchProducerOps;
typedef struct PgBatchProviderOps PgBatchProviderOps;
struct PlanState;

/*
 * Runtime request attached to one batch-aware TupleTableSlot.
 *
 * layout describes logical columns shared by plan nodes. filter_columns and
 * project_columns use these compact column numbers; projected columns are
 * needed only for rows that survive filtering. max_rows is zero when the
 * consumer has no batch-size limit.
 */
typedef struct PgBatchRequest
{
	/* Actual size owned by the bridge; new fields are appended at the end. */
	Size		struct_size;
	const PgBatchLayout *layout;
	const Bitmapset *filter_columns;
	const Bitmapset *project_columns;
	/* True when the parent consumes the slot as a batch, not as scalar rows. */
	bool		return_batch;
	/* Maximum rows accepted in one batch, or zero for no explicit limit. */
	int			max_rows;
}			PgBatchRequest;

#define PG_BATCH_REQUEST_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchRequest, max_rows)

/*
 * Bridge entry points published through PG_BATCH_RENDEZVOUS. The table
 * is owned by the bridge and remains valid for the lifetime of the backend.
 */
typedef struct PgBatchAPI
{
	/* Set to PG_BATCH_API_ABI_VERSION and sizeof(this structure). */
	uint32		abi_version;
	Size		struct_size;

	/* Register a provider table; the bridge borrows it until unregistration. */
	void		(*register_provider) (const PgBatchProviderOps *provider);
	/* Remove the provider with this name after all of its scans have ended. */
	void		(*unregister_provider) (const char *provider_name);
	/* Find the sole provider supporting relation, or NULL; duplicates error. */
	const PgBatchProviderOps *(*find_provider) (Relation relation);
	/* Find a registered provider by its exact stable name, or return NULL. */
	const PgBatchProviderOps *(*get_provider) (const char *provider_name);

	/* Register a batch-producing plan-node implementation. */
	void		(*register_producer) (const PgBatchProducerOps *producer);
	/* Remove a producer after all plans using it have finished. */
	void		(*unregister_producer) (const char *producer_name);
	/* Find the sole registered producer claiming path, or return NULL. */
	const PgBatchProducerOps *(*find_producer) (const Path *path);
	/* Find a producer by the stable name stored in a parent plan. */
	const PgBatchProducerOps *(*get_producer) (const char *producer_name);

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
									const PgBatchConsumerOps *consumer_ops);
	/* Replace the request before it is sealed; masks are copied by the bridge. */
	void		(*set_request) (PgBatchBinding *binding,
								 const Bitmapset *filter_columns,
								 const Bitmapset *project_columns,
								 bool return_batch,
								 int max_rows);
	/* Store borrowed provider identity and execution state on the binding. */
	void		(*set_provider) (PgBatchBinding *binding,
								  const char *provider_name,
								  void *provider_state);
	/* Return the bound provider state and optionally its stable name. */
	void	   *(*get_bound_provider) (PgBatchBinding *binding,
									 const char **provider_name);
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
	void		(*finish_batch) (PgBatchBinding *binding);
	/* Return true when no unfinished batch remains on this binding. */
	bool		(*batch_finished) (PgBatchBinding *binding);
	/* Remove and release the active batch while retaining the slot binding. */
	void		(*clear_batch) (PgBatchBinding *binding);
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
