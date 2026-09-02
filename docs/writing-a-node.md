# Writing a batch-aware plan node

This guide describes the smallest useful path for adding a plan node to
`pg_batch`. The complete independent example is
[`examples/limit_node/`](../examples/limit_node/).

## The execution model

PostgreSQL still calls `ExecProcNode()` and still receives a
`TupleTableSlot *`. A batch-aware child returns a slot attached to the bridge.
The attachment carries an active `PgBatch` when the parent requested batch
output. No new executor entry point is required.

A node normally works with four public objects:

1. `PgBatchNodeOps` lets a parent recognize the node, obtain its logical
   column layout, and configure its request binding.
2. `PgBatchInput` owns the parent side of one child connection.
3. `PgBatchColumnAccess` prepares a set of columns for a set of selected rows,
   then returns native or Datum views of those columns.
4. `PgBatchOutput` publishes a new batch and supplies a scalar row view when
   the parent is not batch-aware.

`PgBatchUnary` combines the input and output lifecycle for the common case
where a one-child node only removes rows from the child's selection. Use it
for filters, limits, and similar nodes. Use `PgBatchInput` and
`PgBatchOutput` directly when a node creates a new physical batch, changes the
column layout, or has more than one child.

## Columns, requests, and lazy work

Batch columns use compact zero-based numbers. They are independent of table
attribute numbers and target-list positions. `PgBatchLayout.target_columns`
maps target-list positions to compact columns.

A parent sends a `PgBatchRequestSpec` to each child:

- `filter_columns` are needed before the current row selection is final;
- `project_columns` are needed only for rows that survive filtering;
- `output_mode` asks for a batch or ordinary scalar rows;
- `max_rows` limits the physical size of a returned batch, or is zero.

Keep filter and projection columns separate. A source may decode filter
columns first, remove rows, and avoid decoding projection-only columns for an
empty batch.

The request is mutable during executor initialization and becomes immutable
when execution starts. Configure it before the first `ExecProcNode()` call.
The bridge copies both column masks, so temporary `Bitmapset` values may be
freed after `set_request()`.

## Row selection

`PgBatchSelection` contains the physical row count and a bitmap of active
rows. Every node may only clear bits. It must never restore a row removed by a
child. Iterate and update it with:

```c
int row = -1;

while ((row = pg_batch_selection_next(&batch->selection, row)) >= 0)
{
	if (!keep_row(row))
		pg_batch_selection_clear(&batch->selection, row);
}
```

The selection may contain more than one 64-bit word. A node that deliberately
supports only 64-row batches must validate that restriction.

## Accessing columns

Prepare a column group once, then obtain the required views:

```c
PgBatchColumnAccess access;
PgBatchInt4Vector column;
Bitmapset *columns = bms_make_singleton(column_number);

pg_batch_column_access_init(&access, batch, columns,
	&batch->selection, PG_BATCH_COLUMN_FILTER);
pg_batch_column_get_int4(&access, column_number, &column);
pg_batch_filter_int4(&column, &batch->selection,
	PG_BATCH_INT4_GT, 10, true);
```

`pg_batch_column_get_int4()` first asks for the optional native int4
interface. It falls back to `PgBatchDatumVector` when the source has no such
view. Generic runtime code should use `pg_batch_column_get_datum()` and must
not depend on Arrow or a particular PostgreSQL type.

Every physical format must implement `PgBatchOps.get_datum_column()`. Native
interfaces are optional fast paths discovered by a stable name and version.
A native interface belongs to the batch operations table, not to one batch,
so discovery is cached.

Borrowed column views remain valid only until the source next prepares
columns or releases the batch, unless that native interface documents a
longer lifetime.

## Building paths and plans

The extension still owns its planner hooks, path selection, and costing. The
runtime library only removes the mechanical `CustomPath` and `CustomScan`
setup. Start with the ordinary path whose planner properties should be kept:

```c
PgBatchPathConfig config =
	PG_BATCH_STRUCT_INITIALIZER(PgBatchPathConfig);

config.template_path = &limit->path;
config.methods = &limit_path_methods;
config.children = list_make1(limit->subpath);
config.expressions = list_make2(limit->limitOffset, limit->limitCount);
return pg_batch_path_create(&config);
```

`pg_batch_path_create()` copies rows, costs, path keys, parameterization, and
parallel properties from `template_path`. It also keeps expressions and
provider data in a `copyObject()`-safe form. Read them in `PlanCustomPath`
and describe the final scan explicitly:

```c
PgBatchPathInfo path = PG_BATCH_STRUCT_INITIALIZER(PgBatchPathInfo);
PgBatchPlanConfig config =
	PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanConfig);

pg_batch_path_get_info(best_path, &path);
config.api = my_api;
config.methods = &limit_plan_methods;
config.layout_policy = PG_BATCH_LAYOUT_PRESERVE_CHILD;
config.layout_child = 0;
config.expressions = path.expressions;
return pg_batch_plan_create(rel, best_path, tlist, custom_plans, &config);
```

The output layout has three policies:

- `PG_BATCH_LAYOUT_PRESERVE_CHILD` forwards one batch-aware child's layout;
- `PG_BATCH_LAYOUT_DENSE` publishes one column per final target entry;
- `PG_BATCH_LAYOUT_EXPLICIT` uses a supplied `PgBatchLayout`.

Use `PRESERVE_CHILD` only for a pass-through node whose target list stays
unchanged. A node with `CUSTOMPATH_SUPPORT_PROJECTION` normally uses `DENSE`,
which follows a projection that PostgreSQL installs after `PlanCustomPath`.

The helper discovers every batch-aware child through the bridge and stores
its stable node name. A NULL child name means that the child returns ordinary
rows. Use `pg_batch_plan_child()` when provider-specific planning needs a
child's plan, node operations, or layout.

Qualifiers and executable expressions are never inferred. Set `qual` and
`expressions` to exactly what the executor will evaluate. This prevents a
wrapper around a complete child path from evaluating its clauses twice.

During executor initialization, read the common information once:

```c
PgBatchPlanInfo plan = PG_BATCH_STRUCT_INITIALIZER(PgBatchPlanInfo);

pg_batch_plan_get_info(scan, &plan);
input = pg_batch_input_create(query_context, child,
	plan.child_names[0]);
output = pg_batch_output_create(query_context, result_slot,
	&plan.layout);
```

Set `PgBatchNodeOps.get_layout` to `pg_batch_plan_get_layout` when every plan
claimed by that operation table is built by `pg_batch_plan_create()`.

Provider-specific `node_data` may still use the named plan codec. The common
plan stores that value without interpreting it. The reader rejects a wrong
kind or version, duplicate fields, wrong field types, missing fields, fields
read twice, and unread fields. Increment the provider record version when its
required fields or meaning changes.

Direct `CustomPath` and `CustomScan` construction remains available for
composite nodes. The built-in scan/filter pair uses it because one custom path
creates two stacked plan nodes.

## Registering the node

Publish one static `PgBatchNodeOps` table:

```c
static const PgBatchNodeOps my_node_ops = {
	PG_BATCH_ABI_INITIALIZER(PG_BATCH_NODE_OPS_ABI_VERSION,
		PgBatchNodeOps),
	.node_name = "my_extension.node",
	.supports_path = supports_path,
	.get_layout = get_layout,
	.get_request_binding = get_request_binding,
};
```

Register it in `_PG_init()` after loading the bridge and unregister it in
`_PG_fini()`. `node_name` is a stable plan-time identity; do not use a pointer
or a load-order-dependent value in saved plan data.

`get_layout()` describes the columns published by the plan. A parent stores
the child node name in its own plan data and passes it to
`pg_batch_input_create()` during executor initialization.

## The short path: a unary node

`examples/limit_node/` shows the recommended implementation.

The planner does three things:

1. It calls `find_node(child_path)` to require a batch-aware child.
2. It uses `pg_batch_path_create()` to retain the Limit expressions.
3. Its `PlanCustomPath` uses `PG_BATCH_LAYOUT_PRESERVE_CHILD`; the common
   plan helper stores the child name, layout, and executable expressions.

The executor reads that data and creates one `PgBatchUnary`:

```c
PgBatchUnaryConfig config = {
	.struct_size = sizeof(config),
	.parent_context = estate->es_query_cxt,
	.node = node,
	.estate = estate,
	.eflags = eflags,
	.child_name = data.node_name,
	.layout = &layout,
	.project_columns = project_columns,
	.row_mode = PG_BATCH_UNARY_COPY_ROW,
	.process = trim_batch,
	.private_data = state,
};

state->unary = pg_batch_unary_create(&config);
```

The `process` callback receives one active child batch. `trim_batch()` in the
example only clears selection bits and returns the remaining row count. It
calls `pg_batch_unary_stop()` once the Limit is satisfied, preventing another
child fetch.

The remaining executor callbacks are small:

```c
return pg_batch_unary_exec(state->unary);
pg_batch_unary_rescan(state->unary);
pg_batch_unary_end(state->unary);
```

`PgBatchUnary` combines local and parent column requests, asks the child for
batch output, handles empty batches, adapts to scalar parents, checks finish
ordering, and records input/output statistics.

Choose `PG_BATCH_UNARY_INPUT_ROW` when the node's scalar result may use the
child slot directly. Choose `PG_BATCH_UNARY_COPY_ROW` when the node declares
its own virtual result slot or changes the target mapping.

## Nodes that create batches

Create a `PgBatchInput` for every batch-aware child. Before execution, send a
request with `pg_batch_input_set_request()`. Then follow this lifecycle:

```text
input_next -> inspect or transform active batch -> input_finish -> input_next
```

`pg_batch_input_next()` rejects a new fetch while the previous batch is still
active, unless a forwarding parent already finished it through the bridge.
Call `pg_batch_input_rescan()` after rescanning the child.

Create a `PgBatchOutput` when the node owns a new physical batch. The output
lifecycle is:

```text
output_publish -> parent consumes -> output_finished -> output_clear
```

`pg_batch_output_publish()` clears a previously finished result and rejects
overlapping active batches. `pg_batch_output_finish()` is only for acting on
behalf of a scalar consumer. End and rescan paths call
`pg_batch_output_clear()` to release any active batch.

Use `PgBatchBuilder` to collect rows from a scalar child into owned,
column-major Datum arrays. It copies pass-by-reference values into its own
memory context:

```c
PgBatchBuilder *builder = pg_batch_builder_create(
	&(PgBatchBuilderConfig) {
		.struct_size = sizeof(PgBatchBuilderConfig),
		.parent_context = estate->es_query_cxt,
		.tuple_desc = ExecGetResultType(child),
		.ncolumns = ncolumns,
		.capacity = 64,
	});
```

Reset it, append slots, and finish it. The returned batch remains valid until
the next reset.

## Parallel execution

Parallelism does not change the batch contract between plan nodes. Build one
parallel-aware path for each batch node in the subtree and let PostgreSQL put
one ordinary `Gather` above that subtree. Workers may then pass batches through
several joins or a partial aggregate without an intermediate scalar row
boundary. A global partial aggregate emits one transition-state row per
participant; only those rows cross `Gather`, and PostgreSQL's final aggregate
combines them.

A directly pulled scan source may append `PgBatchSourceParallelOps` to its
operation table. The callbacks estimate and initialize source-owned DSM state,
attach workers, and reset allocation state for a rescan. They only divide
source work between participants; `begin_scan()` and `end_scan()` still own
backend-local state. Leaving the pointer NULL keeps the source serial. The
heap source uses PostgreSQL's parallel table scan API internally. A source
that publishes through table-AM slot callbacks needs to coordinate the table
AM scan as well and is not covered by these pull-source callbacks.

Stateful nodes still use the standard `CustomExecMethods` DSM callbacks. For
example, `PgBatchHashJoin` stores immutable column-major build chunks in query
DSA and links them from a shared atomic bucket array. Its parallel spill uses
one file namespace per participant and partition, followed by a barrier and
single-owner partition claims. This keeps node policy out of the bridge and
the reusable spill library. If a probe algorithm has a distinct-key fast path,
derive that property from all participants' build data before probing; a
planner estimate is not a correctness guarantee.

Release objects that refer to DSM or `SharedFileSet` from
`ShutdownCustomScan`, while the mapping still exists. `EndCustomScan` may run
after PostgreSQL has detached the parallel context and should only be a
fallback for execution without DSM.

## Checklist

- Keep plan payloads typed and `copyObject()`-compatible.
- Preserve the child's compact column layout or publish a correct new one.
- Keep filter and projection requests separate.
- Clear selection bits; never restore them.
- Use `PgBatchColumnAccess` once per prepared column group.
- Provide Datum fallback for every output format.
- Keep borrowed batches and column views alive until release.
- Finish an input before fetching another batch.
- Clear outputs in both end and rescan paths.
- Support scalar parents, or reject them explicitly during planning.
- Reject unsupported executor flags such as backward scan or mark/restore.
- Add regression tests for batch and scalar parents, empty selections,
  rescans, early stop, NULL values, and lazy projection.
- Benchmark the hot path against the previous implementation.
- Keep one `Gather` above a parallel batch subtree; do not add scalar
  boundaries between batch-aware nodes.
- Destroy DSM-backed local objects from `ShutdownCustomScan`.
