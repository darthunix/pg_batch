#ifndef PG_BATCH_H
#define PG_BATCH_H

#include "access/heap_deform.h"
#include "access/htup.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "nodes/bitmapset.h"
#include "nodes/extensible.h"
#include "nodes/primnodes.h"
#include "storage/block.h"
#include "storage/buf.h"

#include "bridge.h"
#include "node.h"
#include "source.h"
#include "arrow.h"
#include "runtime.h"
#include "kernels.h"
#include "expr.h"

#define PG_BATCH_SIZE 64
#define PG_BATCH_ALL_ROWS UINT64_MAX

#define PG_BATCH_FILTER_PHASE PG_BATCH_COLUMN_FILTER
#define PG_BATCH_PROJECT_PHASE PG_BATCH_COLUMN_PROJECT

typedef PgBatchColumnPhase PgBatchMaterializePhase;

typedef struct PgBatchColumn
{
	Datum	   *values;
	bool	   *isnull;
	/* Heap batches contain at most 64 rows. */
	uint64		valid_rows;
} PgBatchColumn;

typedef struct PgBatchSlot
{
	TupleTableSlot base;
	PgBatchBinding *binding;
	PgBatch *active_batch;

	MemoryContext batch_context;
	/* Map compact slot columns back to physical table attributes. */
	TupleDesc	source_desc;
	AttrNumber *source_attnums;
	int			ncolumns;
	int			nfilter_columns;

	/* Default heap representation: visible tuples from one pinned page. */
	Buffer		buffer;
	BlockNumber block;
	OffsetNumber item_offsets[PG_BATCH_SIZE];
	HeapTupleDeformState cursors[PG_BATCH_SIZE];
	PgBatchColumn *columns;
	PgBatch heap_batch;
	uint64		heap_selection;
	int			current_row;

	/* Heap tuple formed only when a scalar consumer asks for one. */
	HeapTuple	materialized_tuple;

	uint64		filter_datums;
	uint64		project_datums;
	uint64		restarted_project_datums;
} PgBatchSlot;

typedef enum PgBatchAggKind
{
	PG_BATCH_AGG_GROUP_KEY = 1,
	PG_BATCH_AGG_COUNT_STAR,
	PG_BATCH_AGG_COUNT_COLUMN,
	PG_BATCH_AGG_SUM_INT4,
	PG_BATCH_AGG_MIN_INT4,
	PG_BATCH_AGG_MAX_INT4
} PgBatchAggKind;

typedef enum PgBatchHeapScanMode
{
	PG_BATCH_HEAP_SEQ,
	PG_BATCH_HEAP_BITMAP
} PgBatchHeapScanMode;

extern bool pg_batch_enable;
extern bool pg_batch_enable_hash_join;
extern bool pg_batch_enable_simd;
extern double pg_batch_bitmap_min_rows_per_page;
extern const TupleTableSlotOps PgBatchSlotOps;
extern const PgBatchAPI *pg_batch_api;

static inline uint64
pg_batch_nrows_mask(int nrows)
{
	Assert(nrows > 0 && nrows <= PG_BATCH_SIZE);
	return PG_BATCH_ALL_ROWS >> (PG_BATCH_SIZE - nrows);
}

static inline Node *
pg_batch_strip_relabel(Node *node)
{
	if (IsA(node, RelabelType))
		return (Node *) castNode(RelabelType, node)->arg;
	return node;
}

static inline PgBatchSlot *
pg_batch_slot_cast(TupleTableSlot *slot)
{
	if (slot == NULL || slot->tts_ops != &PgBatchSlotOps)
		elog(ERROR, "pg_batch expected its custom tuple slot");
	return (PgBatchSlot *) slot;
}

static inline PgBatch *
pg_batch_get_batch(TupleTableSlot *slot)
{
	PgBatchSlot *bslot = pg_batch_slot_cast(slot);
	PgBatch *batch = bslot->active_batch;

	if (batch == NULL)
		elog(ERROR, "pg_batch expected a published batch");
	return batch;
}

static inline void
pg_batch_finish_batch(TupleTableSlot *slot)
{
	PgBatchBinding *binding = pg_batch_api->find_binding(slot);

	if (binding == NULL)
		elog(ERROR, "pg_batch expected a batch binding");
	pg_batch_api->finish_batch(binding);
}

extern void pg_batch_configure_slot(PgBatchSlot *bslot,
									TupleDesc source_desc,
									List *source_attnums,
									int nfilter_columns);
extern void pg_batch_set_request(TupleTableSlot *slot,
								 const Bitmapset *filter_columns,
								 const Bitmapset *project_columns,
								 bool return_batch);
extern void pg_batch_load_batch(PgBatchSlot *bslot, Buffer buffer,
								BlockNumber block, Oid table_oid,
								const OffsetNumber *item_offsets, int nrows);
extern void pg_batch_select_row(TupleTableSlot *slot, int row);

extern Node *pg_batch_create_scan_state(CustomScan *cscan);
extern Node *pg_batch_create_filter_state(CustomScan *cscan);
extern Node *pg_batch_create_pack_state(CustomScan *cscan);
extern Node *pg_batch_create_project_state(CustomScan *cscan);
extern Node *pg_batch_create_hash_join_state(CustomScan *cscan);
extern TupleTableSlot *pg_batch_hash_join_output_slot(CustomScanState *state);
extern Node *pg_batch_create_agg_state(CustomScan *cscan);

/* Return the batch-producing slot of a pg_batch plan node. */
extern TupleTableSlot *pg_batch_result_batch_slot(PlanState *planstate);

extern void pg_batch_init_children(CustomScanState *css, EState *estate,
								   int eflags);
extern void pg_batch_end_children(CustomScanState *css);
extern void pg_batch_rescan_children(CustomScanState *css);

extern void pg_batch_planner_init(void);
extern void pg_batch_planner_fini(void);

extern const PgBatchProducerOps pg_batch_producer_ops;

#define PG_BATCH_PRODUCER_NAME "pg_batch_nodes"

#endif							/* PG_BATCH_H */
