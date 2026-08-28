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

#include "commands/explain.h"
#include "port/pg_bitutils.h"
#include "executor/tuptable.h"
#include "nodes/bitmapset.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "utils/rel.h"

/*
 * Change both values when an incompatible layout or calling convention is
 * introduced. Extensible operation tables and published batches carry the
 * ABI version and their actual size so the bridge can reject incompatible
 * objects before calling through them.
 */
#define PG_BATCH_BRIDGE_RENDEZVOUS "pg_batch_bridge_api_v1"
#define PG_BATCH_BRIDGE_ABI_VERSION 1

/*
 * Why a column is being prepared or converted to Datum values. Providers may
 * use this hint to keep filter work separate from work needed only for rows
 * that survive filtering.
 */
typedef enum PgBatchBridgeMaterializePhase
{
	/* The rows and columns are needed to evaluate scan filters. */
	PG_BATCH_BRIDGE_MATERIALIZE_FILTER,
	/* The columns are needed only after filtering, for an upper node. */
	PG_BATCH_BRIDGE_MATERIALIZE_PROJECT
}			PgBatchBridgeMaterializePhase;

/*
 * How completely a provider can handle one scan restriction. The result
 * determines whether the executor must keep a local copy of the restriction.
 */
typedef enum PgBatchBridgeQualSupport
{
	/* The provider cannot use the restriction. */
	PG_BATCH_BRIDGE_QUAL_UNSUPPORTED,
	/* The provider may prune storage, but the executor must recheck all rows. */
	PG_BATCH_BRIDGE_QUAL_PRUNE_ONLY,
	/* The provider applies the restriction exactly; no recheck is required. */
	PG_BATCH_BRIDGE_QUAL_EXACT
}			PgBatchBridgeQualSupport;

/* A source-owned batch published through a slot; defined below. */
typedef struct PgBatchBridgeBatch PgBatchBridgeBatch;

/*
 * Opaque bridge-owned association between a TupleTableSlot, its batch
 * request, and its active batch. It is owned by the slot's memory context.
 */
typedef struct PgBatchBridgeBinding PgBatchBridgeBinding;

/*
 * Borrowed view of one column in PostgreSQL Datum form.
 *
 * Arrays are indexed by the row number in PgBatchBridgeBatch. A set bit in
 * valid_rows means that both values[] and isnull[] are available for that
 * row. The view and its arrays remain owned by the source and are valid until
 * the source releases or rematerializes the active batch.
 */
typedef struct PgBatchBridgeDatumColumn
{
	const Datum *values;
	const bool *isnull;
	const uint64 *valid_rows;
	/* Number of uint64 words in valid_rows. */
	int			nwords;
} PgBatchBridgeDatumColumn;

/*
 * Operations supplied by a source for the representation of a batch. Column
 * numbers use the compact layout from PgBatchBridgeRequest. Each rows argument
 * points to batch->nwords bitmap words indexed by batch row number. The table
 * must remain valid while any published batch refers to it.
 */
typedef struct PgBatchBridgeBatchOps
{
	/* Set to PG_BATCH_BRIDGE_ABI_VERSION and sizeof(this structure). */
	uint32		abi_version;
	Size		struct_size;

	/*
	 * Optionally prepare the requested compact columns for the selected rows.
	 * This may decode or cache a source-native representation, but need not
	 * produce Datums. The source may prepare more data than requested and must
	 * not change the batch selection. A NULL callback means no preparation is
	 * needed.
	 */
	void		(*prepare_columns) (PgBatchBridgeBatch *batch,
									const Bitmapset *columns,
									const uint64 *rows,
									PgBatchBridgeMaterializePhase phase);
	/*
	 * Make every requested row available as a Datum and return a borrowed
	 * column view. Implementations may retain earlier results or materialize a
	 * superset. This is the mandatory, format-neutral fallback operation.
	 */
	void		(*get_datum_column) (PgBatchBridgeBatch *batch, int column,
										 const uint64 *rows,
										 PgBatchBridgeMaterializePhase phase,
										 PgBatchBridgeDatumColumn *result);
	/*
	 * Return a borrowed source-native interface for an exact stable name and
	 * version, or NULL when the format is not supported. The returned table
	 * must remain valid for the lifetime of the active batch.
	 */
	const void *(*get_native_interface) (PgBatchBridgeBatch *batch,
											 const char *name,
											 uint32 version);
	/*
	 * Release source resources for a batch removed from its binding. This may
	 * be NULL when the batch owns no resources that need explicit release.
	 */
	void		(*release) (PgBatchBridgeBatch *batch);
}			PgBatchBridgeBatchOps;

/*
 * Common envelope for one active batch, independent of its physical format.
 *
 * The source owns this structure, its selection bitmap, and private_data. All
 * of them must remain valid from publish_batch() until the bridge clears the
 * batch, calling the release operation when present. A source must not reuse
 * or publish the batch again while it is active in a binding.
 */
struct PgBatchBridgeBatch
{
	/* Set to PG_BATCH_BRIDGE_ABI_VERSION and sizeof(this structure). */
	uint32		abi_version;
	Size		struct_size;
	/* Total physical rows in the batch, including rows later deselected. */
	int			nrows;
	/* Number of words in selection; always ceil(nrows / 64). */
	int			nwords;
	/* Mutable survivor bitmap; one means live, and bits beyond nrows are zero. */
	uint64	   *selection;
	/* Relation identity exposed by the scalar TupleTableSlot view. */
	Oid			table_oid;
	/* Set false on publication and true when a batch-aware parent finishes. */
	bool		consumed;
	/* Immutable operations for the source-owned physical representation. */
	const		PgBatchBridgeBatchOps *ops;
	/* Opaque source state passed back to the batch operations. */
	void	   *private_data;
};

/*
 * Operations implemented by the TupleTableSlot that consumes batches. They
 * let the bridge expose a published batch through an otherwise ordinary slot.
 * The operation table must outlive every binding that refers to it.
 */
typedef struct PgBatchBridgeConsumerOps
{
	/* Set to PG_BATCH_BRIDGE_ABI_VERSION and sizeof(this structure). */
	uint32		abi_version;
	Size		struct_size;
	/*
	 * Record a newly published batch as the slot's active batch and initialize
	 * its scalar view. The bridge has installed the batch in the binding before
	 * making this call.
	 */
	void		(*accept_batch) (TupleTableSlot *slot,
								 PgBatchBridgeBatch *batch);
	/*
	 * Change the ordinary slot view to one selected row without taking
	 * ownership of the batch. Previously materialized scalar values must no
	 * longer be exposed as values of the new row.
	 */
	void		(*select_row) (TupleTableSlot *slot,
								   PgBatchBridgeBatch *batch, int row);
}			PgBatchBridgeConsumerOps;

/*
 * Runtime request attached to one batch-aware TupleTableSlot.
 *
 * source_attnums maps each zero-based compact batch column to a one-based
 * physical relation attribute. filter_columns and survivor_columns use
 * compact column numbers; the latter are needed only for rows that survive
 * filtering. The bridge owns copies of the mapping and both bitmaps for the
 * lifetime of the binding.
 *
 * This differs from PgBatchBridgePlanRequest: this structure describes the
 * final slot layout and execution demand, not the planner's proposed clauses
 * and physical relation columns.
 */
typedef struct PgBatchBridgeRequest
{
	const AttrNumber *source_attnums;
	int			ncolumns;
	const Bitmapset *filter_columns;
	const Bitmapset *survivor_columns;
	/* True when the parent consumes the slot as a batch, not as scalar rows. */
	bool		return_batch;
	/* Provider identity and opaque execution state, set after begin_scan(). */
	const char *provider_name;
	void	   *provider_state;
}			PgBatchBridgeRequest;

/*
 * Borrowed planner input passed to one provider's plan_scan() callback.
 *
 * clauses contains bare relation restrictions in executor order.
 * restrictinfos contains the corresponding RestrictInfo nodes in exactly the
 * same order. Providers that move work below the scan boundary must use them
 * to preserve PostgreSQL's security ordering. Attribute bitmaps contain
 * one-based physical relation attribute numbers; projection attributes
 * already used by filters are omitted from project_attnums. The provider must
 * not modify this structure or retain pointers from it after plan_scan()
 * returns.
 */
typedef struct PgBatchBridgePlanRequest
{
	PlannerInfo *root;
	RelOptInfo *rel;
	Relation	relation;
	List	   *clauses;
	List	   *restrictinfos;
	const Bitmapset *filter_attnums;
	const Bitmapset *project_attnums;
}			PgBatchBridgePlanRequest;

/*
 * Provider output from plan_scan(), allocated in the current planner context.
 *
 * qual_support has one entry for every input clause and controls which
 * restrictions remain in executor nodes. source_exprs contains expressions
 * that PostgreSQL must process with setrefs before execution. source_private
 * is the provider's copyObject()-compatible plan payload stored in the
 * CustomScan plan.
 */
typedef struct PgBatchBridgePlanResult
{
	int			nquals;
	PgBatchBridgeQualSupport *qual_support;
	List	   *source_exprs;
	Node	   *source_private;
}			PgBatchBridgePlanResult;

/*
 * Stable mapping from a plan node's output tuple to its published batch.
 *
 * batch_columns has one zero-based compact batch column for every target
 * entry in the Plan. The producer allocates the array in the current planner
 * context; a consumer must copy the values it keeps in its own plan data.
 */
typedef struct PgBatchBridgeOutputLayout
{
	int			ncolumns;
	const int  *batch_columns;
} PgBatchBridgeOutputLayout;

/*
 * Operations implemented by an extension whose plan nodes publish batches.
 *
 * This is separate from PgBatchBridgeProviderOps: a provider supplies batches
 * at a scan boundary, while a producer describes a complete executor node
 * whose output can be consumed as batches by another extension. When its
 * request has return_batch set, every nonempty ExecProcNode() result must be
 * a bridge-attached slot with an active batch. It need not be the request
 * binding's slot: pass-through nodes may return a child's slot.
 */
typedef struct PgBatchBridgeProducerOps
{
	/* Set to PG_BATCH_BRIDGE_ABI_VERSION and sizeof(this structure). */
	uint32		abi_version;
	Size		struct_size;
	/* Stable name stored in parent plans and used again during execution. */
	const char *producer_name;
	/* Return true when this Path will publish batches through the bridge. */
	bool		(*supports_path) (const Path *path);
	/* Describe how Plan target entries map to compact batch columns. */
	void		(*get_output_layout) (const Plan *plan,
								  PgBatchBridgeOutputLayout *result);
	/*
	 * Return the binding through which a parent configures this node's batch
	 * request. The slot returned by ExecProcNode() may have a different
	 * binding; consumers must find the active batch from that returned slot.
	 */
	PgBatchBridgeBinding *(*get_request_binding) (struct PlanState *planstate);
} PgBatchBridgeProducerOps;

/*
 * Borrowed execution input passed once to begin_scan() for a provider selected
 * at planning time. source_private is the saved provider plan; source_exprs
 * contains its executor-ready expressions. slot_request remains valid until
 * the slot binding is detached. A provider should allocate returned state in
 * query_context or one of its children and release it in end_scan().
 */
typedef struct PgBatchBridgeExecRequest
{
	Relation	relation;
	/* Parent scan node used to initialize provider expressions. */
	PlanState  *parent;
	Node	   *source_private;
	List	   *source_exprs;
	MemoryContext query_context;
	const		PgBatchBridgeRequest *slot_request;
}			PgBatchBridgeExecRequest;

/*
 * Operations implemented by an independently built batch source. The table
 * is registered once per backend, must have a unique stable name, and must
 * remain valid until it is unregistered. A scan follows this lifecycle:
 * plan_scan(), begin_scan(), zero or more rescan() calls, then end_scan().
 */
typedef struct PgBatchBridgeProviderOps
{
	/* Set to PG_BATCH_BRIDGE_ABI_VERSION and sizeof(this structure). */
	uint32		abi_version;
	Size		struct_size;
	/* Stable name saved in plans and used to find the provider at execution. */
	const char *provider_name;
	/* Return whether this provider can currently scan the open relation. */
	bool		(*supports_relation) (Relation relation);
	/*
	 * Classify every input restriction and return copyable provider plan data.
	 * The callback must initialize all fields of result, including one support
	 * entry for every input clause.
	 */
	void		(*plan_scan) (const PgBatchBridgePlanRequest * request,
								  PgBatchBridgePlanResult * result);
	/* Start a scan and return opaque state for the remaining callbacks. */
	void	   *(*begin_scan) (const PgBatchBridgeExecRequest * request);
	/*
	 * Return the next source-owned batch, or NULL at end of scan. This is used
	 * by sources such as FDWs that do not have a table AM callback through
	 * which to publish a batch. A NULL callback keeps the existing table-AM
	 * driven path.
	 */
	PgBatchBridgeBatch *(*next_batch) (void *provider_state);
	/* Reset provider state so the same plan can be scanned from the start. */
	void		(*rescan) (void *provider_state);
	/* End the scan and release all resources owned by provider_state. */
	void		(*end_scan) (void *provider_state);
	/* Add provider-specific EXPLAIN properties; this callback is optional. */
	void		(*explain) (void *provider_state, ExplainState *es);
}			PgBatchBridgeProviderOps;

/*
 * Bridge entry points published through PG_BATCH_BRIDGE_RENDEZVOUS. The table
 * is owned by the bridge and remains valid for the lifetime of the backend.
 */
typedef struct PgBatchBridgeAPI
{
	/* Set to PG_BATCH_BRIDGE_ABI_VERSION and sizeof(this structure). */
	uint32		abi_version;
	Size		struct_size;

	/* Register a provider table; the bridge borrows it until unregistration. */
	void		(*register_provider) (const PgBatchBridgeProviderOps * provider);
	/* Remove the provider with this name after all of its scans have ended. */
	void		(*unregister_provider) (const char *provider_name);
	/* Find the sole provider supporting relation, or NULL; duplicates error. */
	const		PgBatchBridgeProviderOps *(*find_provider) (Relation relation);
	/* Find a registered provider by its exact stable name, or return NULL. */
	const		PgBatchBridgeProviderOps *(*get_provider) (const char *provider_name);

	/* Register a batch-producing plan-node implementation. */
	void		(*register_producer) (const PgBatchBridgeProducerOps * producer);
	/* Remove a producer after all plans using it have finished. */
	void		(*unregister_producer) (const char *producer_name);
	/* Find the sole registered producer claiming path, or return NULL. */
	const		PgBatchBridgeProducerOps *(*find_producer) (const Path *path);
	/* Find a producer by the stable name stored in a parent plan. */
	const		PgBatchBridgeProducerOps *(*get_producer) (const char *producer_name);

	/*
	 * Find an existing slot binding without creating one. This lookup is meant
	 * for extension boundaries that receive only TupleTableSlot *, such as a
	 * table access method callback. Batch-aware executors should retain the
	 * returned binding instead of repeating the lookup.
	 */
	PgBatchBridgeBinding *(*find_binding) (TupleTableSlot *slot);
	/*
	 * Attach a new binding to slot and return it. The bridge copies
	 * source_attnums into the slot's memory context and borrows consumer_ops.
	 * Attaching the same slot twice is an error.
	 */
	PgBatchBridgeBinding *(*attach_slot) (TupleTableSlot *slot,
										  const AttrNumber *source_attnums,
										  int ncolumns,
										  const PgBatchBridgeConsumerOps * consumer_ops);
	/* Replace the request masks with bridge-owned copies and set its mode. */
	void		(*set_request) (PgBatchBridgeBinding * binding,
									const Bitmapset *filter_columns,
									const Bitmapset *survivor_columns,
									bool return_batch);
	/* Store the borrowed provider identity and scan state in the request. */
	void		(*set_provider) (PgBatchBridgeBinding * binding,
									 const char *provider_name,
									 void *provider_state);
	/* Return the binding's borrowed, read-only runtime request, or NULL. */
	const		PgBatchBridgeRequest *(*get_request) (PgBatchBridgeBinding * binding);

	/*
	 * Install one source-owned batch and notify the slot consumer. The binding
	 * must not already have an active batch. The bridge validates the common
	 * batch and operation layouts before publishing them.
	 */
	void		(*publish_batch) (PgBatchBridgeBinding * binding,
									  PgBatchBridgeBatch *batch);
	/* Return the binding's active borrowed batch, or NULL. */
	PgBatchBridgeBatch *(*get_batch) (PgBatchBridgeBinding * binding);
	/* Expose one selected batch row through the binding's scalar slot view. */
	void		(*select_row) (PgBatchBridgeBinding * binding, int row);
	/* Mark the active batch consumed so a batch-aware parent may request more. */
	void		(*finish_batch) (PgBatchBridgeBinding * binding);
	/* Remove and release the active batch while retaining the slot binding. */
	void		(*clear_batch) (PgBatchBridgeBinding * binding);
	/* Release any batch, remove the slot attachment, and free the binding. */
	void		(*detach_binding) (PgBatchBridgeBinding * binding);
}			PgBatchBridgeAPI;

/* Return the number of set bits in the active row selection. */
static inline int
pg_batch_bridge_selection_count(const PgBatchBridgeBatch *batch)
{
	int			result = 0;

	if (likely(batch->nwords == 1))
		return pg_popcount64(batch->selection[0]);
	for (int word = 0; word < batch->nwords; word++)
		result += pg_popcount64(batch->selection[word]);
	return result;
}

/* Return whether row is still present in the active selection. */
static inline bool
pg_batch_bridge_row_selected(const PgBatchBridgeBatch *batch, int row)
{
	Assert(row >= 0 && row < batch->nrows);
	return (batch->selection[row / 64] &
			(UINT64CONST(1) << (row % 64))) != 0;
}

/*
 * Remove row from the active selection. Clearing an already clear bit is safe.
 */
static inline void
pg_batch_bridge_clear_row(PgBatchBridgeBatch *batch, int row)
{
	Assert(row >= 0 && row < batch->nrows);
	batch->selection[row / 64] &=
		~(UINT64CONST(1) << (row % 64));
}

/* Return the next selected row; pass -1 to start, and get -1 at the end. */
static inline int
pg_batch_bridge_next_selected(const PgBatchBridgeBatch *batch, int previous)
{
	int			row = previous + 1;
	int			word = row / 64;
	uint64		bits;

	if (row >= batch->nrows)
		return -1;
	bits = batch->selection[word] & (UINT64_MAX << (row % 64));
	for (;;)
	{
		if (bits != 0)
		{
			int			result = word * 64 + pg_rightmost_one_pos64(bits);

			return result < batch->nrows ? result : -1;
		}
		if (++word >= batch->nwords)
			return -1;
		bits = batch->selection[word];
	}
}

#endif							/* PG_BATCH_BRIDGE_H */
