#ifndef PG_BATCH_H
#define PG_BATCH_H

#include "access/heap_deform.h"
#include "access/htup.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "nodes/bitmapset.h"
#include "nodes/extensible.h"
#include "nodes/primnodes.h"
#include "port/pg_bitutils.h"
#include "storage/block.h"
#include "storage/buf.h"

#include "pg_batch_arrow.h"

#define PG_BATCH_SIZE 64
#define PG_BATCH_ALL_ROWS UINT64_MAX

typedef enum PgBatchMaterializePhase
{
	PG_BATCH_FILTER_PHASE,
	PG_BATCH_PROJECT_PHASE
} PgBatchMaterializePhase;

typedef struct PgBatchColumn
{
	Datum	   *values;
	bool	   *isnull;
	/* Rows for which values[] and isnull[] have been filled. */
	uint64		valid_rows;
} PgBatchColumn;

typedef struct PgBatchArrowView
{
	const struct ArrowArray *array;
	const struct ArrowSchema *schema;
} PgBatchArrowView;

typedef enum PgBatchCompressedScanMode
{
	PG_BATCH_COMPRESSED_BATCH,
	PG_BATCH_COMPRESSED_PRUNE,
	PG_BATCH_COMPRESSED_FILTER
} PgBatchCompressedScanMode;

typedef enum PgBatchSourceOp
{
	PG_BATCH_SOURCE_UNSUPPORTED,
	PG_BATCH_SOURCE_EQ,
	PG_BATCH_SOURCE_NE,
	PG_BATCH_SOURCE_LT,
	PG_BATCH_SOURCE_LE,
	PG_BATCH_SOURCE_GT,
	PG_BATCH_SOURCE_GE
} PgBatchSourceOp;

typedef struct PgBatchSourceQual
{
	int			column;
	PgBatchSourceOp op;
	NullableDatum scalar;
} PgBatchSourceQual;

typedef struct PgBatchCompressedStats
{
	uint64		groups_examined;
	uint64		groups_skipped_minmax;
	uint64		groups_skipped_membership;
	uint64		rows_removed_by_source_filter;
	uint64		encoded_bytes_touched;
} PgBatchCompressedStats;

struct PgBatchSlot;

typedef struct PgBatchFormatOps
{
	void		(*prepare_columns) (struct PgBatchSlot *bslot,
									const Bitmapset *columns, uint64 rows,
									PgBatchMaterializePhase phase);
	PgBatchArrowView (*get_arrow_column) (struct PgBatchSlot *bslot, int column);
	void		(*clear_batch) (struct PgBatchSlot *bslot);
} PgBatchFormatOps;

typedef struct PgBatchRequest
{
	/*
	 * A parent configures this request before the first ExecProcNode() call.
	 * Column numbers are positions in the compact scan descriptor, not table
	 * attribute numbers.
	 */
	/* Prepare these columns for every source row before filtering. */
	Bitmapset  *filter_columns;
	/* Prepare these columns only for rows that survive filtering. */
	Bitmapset  *survivor_columns;
	/* Return one batch to a batch-aware parent instead of scalar rows. */
	bool		return_batch;

	/*
	 * A batch source may use these restrictions for group pruning and, when
	 * it understands them exactly, row filtering. Unsupported restrictions
	 * are still evaluated by PgBatchScan.
	 */
	const PgBatchSourceQual *source_quals;
	int			nsource_quals;
	PgBatchCompressedScanMode source_mode;
} PgBatchRequest;

typedef struct PgBatchSlot
{
	TupleTableSlot base;

	MemoryContext batch_context;
	/* Map compact slot columns back to physical table attributes. */
	TupleDesc	source_desc;
	AttrNumber *source_attnums;
	int			ncolumns;
	/* Filter columns form a prefix of the compact slot descriptor. */
	int			nfilter_columns;

	/* Default heap representation: visible tuples from one pinned page. */
	Buffer		buffer;
	BlockNumber block;
	Oid			table_oid;
	OffsetNumber item_offsets[PG_BATCH_SIZE];
	/* A high-water deformation cursor for each tuple on the heap page. */
	HeapTupleDeformState cursors[PG_BATCH_SIZE];
	PgBatchColumn *columns;
	/* NULL means the default heap Datum representation. */
	const PgBatchFormatOps *format_ops;
	void	   *format_private;
	int			nrows;
	/* selected_rows is a bit mask; current_row provides its scalar view. */
	int			current_row;
	uint64		selected_rows;
	/* The producer cannot replace the batch until its consumer sets this. */
	bool		batch_consumed;

	PgBatchRequest request;
	/* Opaque state used only while a compressed source owns this slot. */
	void	   *compressed_scan;
	/* Heap tuple formed only when a scalar consumer asks for one. */
	HeapTuple	materialized_tuple;

	uint64		filter_datums;
	uint64		project_datums;
	uint64		restarted_project_datums;
	uint64		arrow_filter_columns;
	uint64		arrow_project_columns;
	uint64		arrow_decoded_values;
} PgBatchSlot;

typedef enum PgBatchAggKind
{
	PG_BATCH_AGG_COUNT_STAR = 1,
	PG_BATCH_AGG_COUNT_COLUMN,
	PG_BATCH_AGG_SUM_INT4
} PgBatchAggKind;

extern bool pg_batch_enable;
extern bool pg_batch_use_compressed;
extern bool pg_batch_compressed_via_tableam;
extern int pg_batch_compressed_scan_mode;
extern const TupleTableSlotOps PgBatchSlotOps;

static inline uint64
pg_batch_nrows_mask(int nrows)
{
	Assert(nrows > 0 && nrows <= PG_BATCH_SIZE);
	return PG_BATCH_ALL_ROWS >> (PG_BATCH_SIZE - nrows);
}

static inline int
pg_batch_row_count(uint64 selected_rows)
{
	return pg_popcount64(selected_rows);
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

static inline void
pg_batch_finish_batch(PgBatchSlot *bslot)
{
	bslot->batch_consumed = true;
}

extern void pg_batch_configure_slot(PgBatchSlot *bslot,
									TupleDesc source_desc,
									List *source_attnums,
									int nfilter_columns);
extern void pg_batch_set_request(PgBatchSlot *bslot,
								 const Bitmapset *filter_columns,
								 const Bitmapset *survivor_columns,
								 bool return_batch);
extern void pg_batch_set_source_request(PgBatchSlot *bslot,
									const PgBatchSourceQual *quals,
									int nquals,
									PgBatchCompressedScanMode mode);
extern void pg_batch_load_batch(PgBatchSlot *bslot, Buffer buffer,
								BlockNumber block, Oid table_oid,
								const OffsetNumber *item_offsets, int nrows);
extern void pg_batch_materialize_columns(PgBatchSlot *bslot,
										 const Bitmapset *columns,
										 uint64 selected_rows,
										 PgBatchMaterializePhase phase);
extern void pg_batch_prepare_columns(PgBatchSlot *bslot,
									 const Bitmapset *columns,
									 uint64 selected_rows,
									 PgBatchMaterializePhase phase);
extern PgBatchArrowView pg_batch_get_arrow_column(PgBatchSlot *bslot,
												  int column);
extern void pg_batch_slot_select_row(PgBatchSlot *bslot, int row);

extern bool pg_batch_compressed_available(Relation relation);
extern bool pg_batch_compressed_scan_next(PgBatchSlot *bslot,
										  Relation relation);
extern void pg_batch_compressed_scan_rescan(PgBatchSlot *bslot);
extern void pg_batch_compressed_scan_end(PgBatchSlot *bslot);
extern const PgBatchCompressedStats *pg_batch_compressed_scan_stats(PgBatchSlot *bslot);
extern void pg_batch_compress_fini(void);

extern bool pg_batch_relation_uses_tableam(Relation relation);

extern OpExpr *pg_batch_match_qual(Node *clause, uint8 *var_argno);
extern Node *pg_batch_create_scan_state(CustomScan *cscan);
extern Node *pg_batch_create_filter_state(CustomScan *cscan);
extern Node *pg_batch_create_agg_state(CustomScan *cscan);

extern void pg_batch_init_children(CustomScanState *css, EState *estate,
								   int eflags);
extern void pg_batch_end_children(CustomScanState *css);
extern void pg_batch_rescan_children(CustomScanState *css);

extern void pg_batch_planner_init(void);
extern void pg_batch_planner_fini(void);

#endif							/* PG_BATCH_H */
