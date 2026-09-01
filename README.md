# pg_batch

`pg_batch` is an executor playground built against PostgreSQL master. It tests
whether independent extensions can exchange batches through ordinary tuple
slots, keep source-native columns between plan nodes, and postpone conversion
to PostgreSQL `Datum` values.

The repository is split into five extensions and four reusable C libraries:

```text
examples/compressed_tam/ -----> bridge/ <----- nodes/
examples/arrow_fdw/ -----------/                 |
examples/limit_node/ ----------/-----------------/
                                                  v
                         runtime/ kernels/ expr/ spill/
```

- `bridge/` is the only shared dependency. It provides a versioned rendezvous
  API, attaches batch requests and active batches to `TupleTableSlot *`, and
  registers independent batch sources. Plan nodes keep the returned opaque
  binding, so normal batch operations do not search for the slot attachment.
- `nodes/` provides `PgBatchScan`, `PgBatchFilterProject`, `PgBatchHashJoin`,
  `PgBatchAgg`, and the custom batch slot. It knows nothing about the test
  table access method.
- `examples/compressed_tam/` provides a heap-compatible test table access
  method and a
  backend-local compressed columnar snapshot. It knows nothing about the
  custom nodes or their private slot structure.
- `examples/arrow_fdw/` is an independent Arrow IPC test source. It has both
  an ordinary
  row-at-a-time FDW path and a direct bridge callback that returns native
  Arrow batches to the same nodes.
- `examples/limit_node/` is an independently built unary node. It uses
  only installed public headers and the runtime library, which keeps the
  node interface honest without depending on `nodes/` internals.
- `runtime/` provides common batch traversal, lazy Datum/native adapters,
  borrowed vector views, an owned builder for collecting scalar slots,
  and opaque input/output objects for connecting batch nodes and
  consumers.
  It also provides `PgBatchUnary` for selection-changing nodes that preserve a
  child's batch. `kernels/` provides type-specific operations over those
  views. `expr/`
  compiles a deliberately small class of PostgreSQL expressions into reusable
  vector operations and provides lazy projected batches. `spill/` provides
  bounded buffered partitions and dense 64-row blocks shared by stateful
  nodes. All four are
  statically linked into consumers, so they add no loaded module or
  cross-extension function-call ABI.

`nodes` and both examples consume the same runtime, kernel, and expression
libraries. The node extension also uses the spill library. The bridge remains
their only shared runtime service; the static libraries only remove duplicated
C implementation from the extensions.

See [`docs/writing-a-node.md`](docs/writing-a-node.md) for the public execution
model and a step-by-step guide to building an independent batch-aware node.

The `.control` and extension SQL files retain their full extension names
because PostgreSQL looks them up by those names. Public headers are installed
under `extension/pg_batch/`; internal source files use short
names such as `planner.c`, `slot.c`, `source.c`, and `compressed.c`.

## Batch contract

At planning time a source classifies each restriction as:

- `EXACT`: the source applies it completely, so the executor removes the local
  copy;
- `PRUNE_ONLY`: the source may skip groups, but the executor still checks the
  condition;
- `UNSUPPORTED`: the condition stays entirely above the source.

Filter attributes and projection-only attributes are passed separately. This
lets a source read filter columns first and materialize projection columns only
for surviving rows.

At execution time the bridge attaches a request to the scan slot. A table
access method may publish through its normal slot callback. A source without
such a callback, including an FDW, may instead return a batch from the optional
source `next_batch` operation; `PgBatchScan` publishes it on the same slot.
Every batch must provide lazy `Datum` materialization, which is the
format-neutral fallback for ordinary PostgreSQL consumers. A batch may also
publish optional named native interfaces. The test columnar sources and
computed projections publish a common `int4` vector view in addition to Arrow
C Data where applicable. Filters and aggregates therefore consume packed
values directly without depending on one source format. Native capabilities
belong to a batch operations table, so the runtime validates and caches each
typed interface once instead of rediscovering it for every batch.

The attachment lookup is only for boundaries that receive an ordinary
`TupleTableSlot *`, such as a table access method callback. It returns an
opaque `PgBatchBinding *`. Batch-aware nodes retain that pointer and use
it directly; the test table access method performs one lookup for each batch it
returns.

Batch consumers use `PgBatchInput` to keep the node, request binding, and
actual returned slot binding together. This matters for unary nodes:
the slot that receives a request need not be the slot that later publishes a
batch. The input object caches that returned binding and provides explicit
next, finish, and rescan transitions without exposing its state.
For scalar expressions it can borrow one `PgBatchRowView` per batch and select
rows through the consumer directly, avoiding a bridge call in the hot loop.
`PgBatchOutput` publishes batches to another batch-aware node or exposes the
selected rows through the same slot to an ordinary row-at-a-time parent.
`PgBatchUnary` implements the common case where a node keeps the child's batch
representation and only changes its row selection. The node supplies one
batch callback; the runtime forwards column and batch-size requests and owns
the scalar/batch, finish, rescan, and instrumentation bookkeeping.

`PgBatchLayout` separates compact batch columns from plan target-list
positions. This preserves hidden filter columns across unary nodes
without making them visible in the tuple descriptor. `PgBatchRequest` uses
that logical layout, separate filter and lazy-projection masks, and an optional
maximum batch size. The request becomes immutable before execution starts, so
a source can safely retain it for the scan lifetime.

A published `PgBatch` has one logical owner. That owner may only remove rows
from `selection`; it cannot restore a bit cleared by an earlier node. Passing
the batch upward transfers this mutation right. Physical buffers may be
shared by separate batch views, but the views and their selection masks remain
separate. Native and Datum column views are borrowed and remain valid only
until the owner next prepares columns or releases the batch.

Every public operation table has its own ABI version and byte size. Version 1
defines a required prefix; optional callbacks are appended and used only when
the reported size includes them. Callback arguments and results that may grow
also carry their byte size. This permits an older extension to run with a
newer bridge when the required prefix is unchanged.

The bridge does not contain table-access-method policy or expression logic. It
only owns the common ABI, source registry, slot attachments, selection bitmap,
and batch lifetime transitions.

## Executor nodes

`PgBatchScan` reads up to 64 visible heap tuples from one pinned page, or asks a
registered source to publish its next batch. Simple restrictions run in dense
column loops. For heap tables the scan can be sequential or use pages selected
by a generic PostgreSQL bitmap. The bitmap may come from B-tree, BRIN, GiST,
GIN, `BitmapAnd`, or `BitmapOr`; index ordering is not preserved. Direct
ordered `Index Scan` and `Index Only Scan` remain row-at-a-time because their
adjacent entries need not refer to the same pinned heap page.

On exact bitmap pages, `PgBatchScan` does not repeat conditions already proved
by the bitmap and does not request early materialization of columns used only
by those conditions. Lossy pages still recheck every index condition. The
`pg_batch.bitmap_min_rows_per_page` setting, which defaults to `8`, prevents an
exact batch bitmap path when the planner expects too few matching rows on each
heap page or when a restriction cannot use a dense column loop; `0` disables
this conservative guard. BRIN-only bitmaps use the table's average rows per
page because BRIN publishes complete lossy pages.

`PgBatchFilterProject` checks residual expressions, updates the selection
bitmap, and requests projection columns only for survivors. Supported filters
contain one `int4` column, constants or execution parameters, `+`, `-`, `*`,
`/`, `%`, unary minus, and one comparison. Conditions are still applied in
planner order. Unsupported conditions remain in PostgreSQL's scalar executor.

`PgBatchProject` computes the same restricted `int4` expressions only when a
batch-aware parent asks for them. Direct columns keep their source-native
representation, duplicate expressions are computed once, and conversion to
Datum stays lazy. `PgBatchAgg` consumes full batches for `count(*)`,
`count(expression)`, `sum(expression)`, `min(expression)`, and
`max(expression)` without turning them into a row stream. Its planner asks the
registered node for the output-column layout, so it is not tied to a
private node or slot type. It also supports one direct `int4` grouping key.
Groups that fit stay in a PostgreSQL hash table; new groups beyond the hash
memory limit are divided into spill partitions and processed later. A
partition that still has too many distinct groups is divided again with the
next hash bits.

`PgBatchHashJoin` is a deliberately narrow `int4` experiment. It supports a
serial, unparameterized inner join with one or more direct `int4 = int4` hash
keys. The build table, probe windows, and output stay columnar. Output columns
are gathered lazily, so an unused projection does not have to be converted to
Datum. Ordinary parents can still consume the same output slot one row at a
time, while `PgBatchAgg` consumes its published batches directly.

The in-memory table uses open addressing for distinct keys and short chains
only for real duplicates. A unique-key build uses a fused lookup/output loop.
Direct safe `int4` conditions between the two inputs are evaluated over the
candidate batch; other join conditions retain the normal scalar expression
fallback.

The join uses PostgreSQL's hash memory limit and the reusable `spill/` library.
When the build side does not fit, the join keeps the partitions that fit in
memory and writes the others as dense 64-row column blocks. Probe rows for
resident partitions are joined immediately. A complete compact key filter
rejects probe rows that cannot match before their projection columns are
materialized or written. Input rows are grouped by partition in one pass, and
small writes are buffered within a caller-provided memory limit. If a partition
would need at least six build chunks, it is partitioned once more with the next
hash bits. Data that remains skewed is read in bounded build chunks, with its
probe partition rescanned for every chunk. The spill library owns only block
storage and readers; partition choice, recursion, and join policy remain in the
node and do not extend the bridge ABI.
Set `pg_batch.enable_hash_join = off` to keep the other batch nodes enabled
while comparing against PostgreSQL's Hash Join.
Set `pg_batch.enable_simd = off` to use the direct scalar `int4` filter kernel
on systems where a SIMD implementation is available.

`PgBatchLimit` is kept in `examples/limit_node/` to demonstrate a node built
against the public API alone. It narrows the input selection across batch
boundaries for ordinary `LIMIT` and `OFFSET`, returns the input batch without
copying to a batch-aware parent, and copies only the current row to its declared
virtual slot for an ordinary parent. `WITH TIES` remains on PostgreSQL's core
Limit path.

For heap batches, the compact slot stores only columns used by the query.
Filter columns precede projection-only columns. Every source row keeps a
`HeapTupleDeformState`, so lazy projection can continue from the saved physical
attribute and byte offset. The included PostgreSQL patch exposes the small
incremental deformation cursor needed for this path.

## Test columnar source

The `pg_batch_compressed` table access method deliberately reuses heap storage
for normal inserts and scalar scans. `pg_batch_compress()` builds a
backend-local experimental snapshot with 64-row column batches, Arrow validity
bitmaps, and `PLAIN`, `DELTA8`, or `DELTA16` encoding for `int4` columns.

Larger storage groups contain per-column min/max values, NULL presence, and an
exact small value set until it overflows. `pg_batch_tam.scan_mode` selects:

- `batch`: return native batches without source filtering;
- `prune`: use group metadata, then keep all restrictions in executor nodes;
- `filter`: prune groups and apply supported `int4` comparisons exactly in the
  source. Arithmetic filters supported by the common expression library can
  run exactly in `filter` mode; group pruning remains limited to direct column
  comparisons.

The snapshot is not durable and is not kept in sync with the heap storage. It
exists only to test the contract between a source and unrelated batch nodes.

## Test Arrow FDW

`pg_batch_fdw` checks the same bridge contract without a table access method.
It reads an Arrow IPC Stream through the vendored nanoarrow 0.9.0 library. Its
planner recognizes securely promotable expressions from the restricted
`int4` language. Supported restrictions run over native Arrow values before a
batch is returned; other expressions stay in `PgBatchFilterProject`.

The reader first decodes columns needed by filters. Projection-only columns
are decoded only for record batches with surviving rows. The IPC reader must
still read each complete record body, so this is CPU-side column pruning, not
physical column I/O pruning. `EXPLAIN ANALYZE` reports record batches, 64-row
windows, removed rows, bytes read, decoded filter and projection columns, and
Datum conversions.

This is intentionally a narrow test format: all foreign columns must be
`int4`, and the stream must use modern uncompressed record batches without
dictionaries. Local file access and the export helper are superuser-only. The
ordinary `ForeignScan` path remains available when `pg_batch.enable` is off.

## Build and test

Meson 0.61 or newer and `pkg-config` are required for the reusable libraries.
On macOS they can be installed with:

```sh
brew install meson pkg-config
```

Apply the included PostgreSQL patch to a master checkout:

```sh
git -C ../postgres am \
    ../pg_batch/patches/postgres/0001-Expose-incremental-heap-tuple-deformation.patch
```

Clone submodules, then build and install all components. The top-level
Makefile asks Meson to build the static libraries before invoking PGXS for the
extensions:

```sh
git submodule update --init
make PG_CONFIG=/path/to/patched/postgres/bin/pg_config
make PG_CONFIG=/path/to/patched/postgres/bin/pg_config install
```

An independent PGXS extension can consume the installed libraries with:

```sh
export PKG_CONFIG_PATH="$(pg_config --pkglibdir)/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pkg-config --cflags --libs --static pg_batch-kernels
```

`pg_batch-kernels` includes the public dependency on `pg_batch-runtime`.
Consumers do not need paths into this source tree.

Extensions that also need the expression compiler can use:

```sh
pkg-config --cflags --libs --static pg_batch-expr
```

`pg_batch-expr` includes the kernel and runtime dependencies.

Stateful nodes can use the bounded partitioned spill blocks with:

```sh
pkg-config --cflags --libs --static pg_batch-spill
```

Create and load them in dependency order:

```sql
CREATE EXTENSION pg_batch_api;
CREATE EXTENSION pg_batch_tam;
CREATE EXTENSION pg_batch_fdw;
CREATE EXTENSION pg_batch;
CREATE EXTENSION pg_batch_limit;
LOAD 'pg_batch_tam';
LOAD 'pg_batch_fdw';
LOAD 'pg_batch';
LOAD 'pg_batch_limit';
```

For a columnar-source example:

```sql
CREATE TABLE measurements(ts int, device int, value int)
USING pg_batch_compressed;

INSERT INTO measurements
SELECT g, g % 16, g * 10 FROM generate_series(1, 100000) AS g;

SELECT pg_batch_compress('measurements');
SET pg_batch_tam.scan_mode = filter;
SELECT count(*), sum(value) FROM measurements WHERE device = 3;
```

Run the regression suite against an existing server with:

```sh
PGPORT=5432 make \
    PG_CONFIG=/path/to/patched/postgres/bin/pg_config installcheck
```

The suite also builds separate C modules through the installed `pkg-config`
metadata, without private node headers. They check the reusable expression
analyzer, kernels, public batch input/output symbols, a bounded spill set, and
a 70-row owned builder containing by-value, copied by-reference, and NULL
values. The SQL tests cover
heap, bitmap-index, table-AM, and FDW batches;
native Arrow consumption; lazy `Datum` fallback; exact and pruning-only
predicates; arithmetic filters and projections; parameters; NULL and error
semantics; grouped and global aggregates; in-memory and spilled joins; rescans;
early stop; disabled-source fallback; ABI rejection; and duplicate source
rejection.

## Source layout

- `include/pg_batch/batch.h` defines the batch value and lazy column contract.
- `include/pg_batch/bridge.h` defines bindings, requests, and the versioned
  registry ABI.
- `include/pg_batch/source.h` defines the planner and executor contract for a
  batch source; `node.h` defines the node-side plan-node contract.
- `include/pg_batch/plan.h` provides named, checked plan data that remains
  compatible with PostgreSQL's `copyObject()`.
- `include/pg_batch/arrow.h` defines the optional Arrow interface.
- `bridge/bridge.c` owns the registry and slot attachments.
- `runtime/` builds `libpg_batch_runtime.a` and provides common selection,
  column access, native/Datum adapters, borrowed vector views, and the
  reusable scalar-to-Datum builder used by `PgBatchPack`. Its opaque
  `PgBatchInput` also implements the common child-node lifecycle.
- `kernels/` builds `libpg_batch_kernels.a` and provides direct `int4`
  comparisons, scalar arithmetic, reductions, and hashing, with optional SIMD
  for dense comparisons.
- `expr/` builds `libpg_batch_expr.a`, analyzes and executes restricted `int4`
  expressions, and exposes a reusable lazy projection batch.
- `spill/` builds `libpg_batch_spill.a` and provides bounded buffered logical
  tape partitions with rewindable or destructive readers.
- `nodes/planner.c` builds sequential and bitmap-backed custom plans.
- `nodes/slot.c` implements the custom slot and heap batch source.
- `nodes/scan.c`, `filter.c`, `project.c`, `hash_join.c`, and `aggregate.c`
  implement the executor nodes.
- `examples/compressed_tam/source.c` classifies source predicates and
  manages scans.
- `examples/compressed_tam/compressed.c` owns snapshots, native columns, group
  pruning, and source
  filtering.
- `examples/compressed_tam/tableam.c` is the heap-compatible test table access
  method boundary.
- `examples/arrow_fdw/planner.c` implements both the PostgreSQL FDW callbacks
  and bridge
  predicate classification.
- `examples/arrow_fdw/scan.c` reads Arrow IPC record batches, applies source
  filters, and
  exposes lazy native or Datum columns.
- `examples/arrow_fdw/export.c` writes ordinary `int4` tables as test Arrow IPC
  streams.

## Benchmark

Create the heap data once, then run the desired comparison scripts:

```sh
psql -f benchmark/setup.sql
psql -f benchmark/run_heap_compare.sql
psql -f benchmark/run_compressed.sql
psql -f benchmark/run_kernels.sql
psql -f benchmark/run_reductions.sql
psql -f benchmark/run_hash_kernel.sql
psql -f benchmark/run_pack.sql
psql -f benchmark/run_input.sql
psql -f benchmark/run_expr.sql
psql -f benchmark/run_mixed.sql
psql -f benchmark/run_groups.sql
psql -f benchmark/run_brin.sql
psql -f benchmark/run_bitmap_index.sql
psql -f benchmark/run_hash_join.sql
psql -f benchmark/run_hash_join_large.sql
psql -f benchmark/run_group_aggregate.sql
psql -f benchmark/run_fdw.sql
```

`run_heap_compare.sql` checks that the bridge refactoring does not slow the
existing heap path. `run_compressed.sql` compares the PostgreSQL executor, heap
batches, and the independent native source. `run_groups.sql` compares full
heap scans, heap batches, BRIN, batch-over-BRIN, native batches, group pruning,
and exact source filtering. Current measurements and query explanations are in
[`benchmark/results.md`](benchmark/results.md).
`run_kernels.sql` compares the direct scalar and optional SIMD int4 filter
kernels over Datum and packed int32 columns.
`run_reductions.sql` compares scalar int4 reduction kernels over dense and
sparse Datum and packed int32 columns.
`run_hash_kernel.sql` compares the current hash join with another library
over one or two heap and packed keys, including sparse, spill, and early-limit
cases. It also covers a scalar `generate_series` input packed below the join.
`run_pack.sql` compares two node libraries while `PgBatchPack` collects one or
two `Datum` columns from a large scalar `generate_series` input.
`run_input.sql` isolates the shared node-input lifecycle with empty and
dense filters, a late projection, and fused aggregates. It accepts the same
`batch_library`, `variant`, and `repetitions` variables.
`run_expr.sql` alternates ordinary and batch execution for arithmetic filters,
computed aggregate inputs, and a late column in a wide heap tuple.
`run_group_aggregate.sql` compares grouped `count`, `sum`, `min`, and `max`
over 1,000 and 200,000 groups, including a low-`work_mem` spill case.

```sh
psql -v variant=before -v batch_library=/path/to/before/pg_batch \
  -f benchmark/run_hash_kernel.sql
psql -v variant=current -v setup=false \
  -v batch_library=/path/to/current/pg_batch \
  -f benchmark/run_hash_kernel.sql
```

`run_bitmap_index.sql` compares ordered `Index Scan`, ordinary
`Bitmap Heap Scan`, and batch-over-bitmap execution for B-tree indexes at
different selectivities and heap orders.
`run_fdw.sql` compares an ordinary heap scan, scalar `ForeignScan`, and direct
batch FDW execution with source pushdown and lazy column decoding enabled or
disabled.
