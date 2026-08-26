# pg_batch

`pg_batch` is an executor playground built against PostgreSQL master. It tests
whether independent extensions can exchange batches through ordinary tuple
slots, keep source-native columns between plan nodes, and postpone conversion
to PostgreSQL `Datum` values.

The repository is split into three components:

```text
nodes/  ----->  bridge/  <-----  tam/
```

- `bridge/` is the only shared dependency. It provides a versioned rendezvous
  API, attaches batch requests and active batches to `TupleTableSlot *`, and
  registers independent batch sources. Plan nodes keep the returned opaque
  binding, so normal batch operations do not search for the slot attachment.
- `nodes/` provides `PgBatchScan`, `PgBatchFilterProject`, `PgBatchAgg`, and the
  custom batch slot. It knows nothing about the test table access method.
- `tam/` provides a heap-compatible test table access method and a
  backend-local compressed columnar snapshot. It knows nothing about the
  custom nodes or their private slot structure.

The `.control` and extension SQL files retain their full extension names
because PostgreSQL looks them up by those names. Public bridge headers are
installed under `extension/pg_batch_bridge/`; internal source files use short
names such as `planner.c`, `slot.c`, `provider.c`, and `compressed.c`.

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
access method or another source reads that request and publishes a batch on the
same slot. Every batch must provide lazy `Datum` materialization, which is the
format-neutral fallback for ordinary PostgreSQL consumers. A batch may also
publish optional named native interfaces. The test columnar source publishes
Arrow C Data views; filters and aggregates consume those buffers directly.

The attachment lookup is only for boundaries that receive an ordinary
`TupleTableSlot *`, such as a table access method callback. It returns an
opaque `PgBatchBridgeBinding *`. Batch-aware nodes retain that pointer and use
it directly; the test table access method performs one lookup for each batch it
returns.

The bridge does not contain table-access-method policy or expression logic. It
only owns the common ABI, source registry, slot attachments, selection bitmap,
and batch lifetime transitions.

## Executor nodes

`PgBatchScan` reads up to 64 visible heap tuples from one pinned page, or asks a
registered source to publish its next batch. Simple restrictions run in dense
column loops. For heap tables the scan can be sequential or use pages selected
by a BRIN bitmap.

`PgBatchFilterProject` checks residual expressions, updates the selection
bitmap, and requests projection columns only for survivors. `PgBatchAgg`
consumes full batches for `count(*)`, `count(column)`, and `sum(int4)` without
turning them into a row stream.

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
  source.

The snapshot is not durable and is not kept in sync with the heap storage. It
exists only to test the contract between a source and unrelated batch nodes.

## Build and test

Apply the included PostgreSQL patch to a master checkout:

```sh
git -C ../postgres am \
    ../pg_batch/patches/postgres/0001-Expose-incremental-heap-tuple-deformation.patch
```

Then build and install all three components:

```sh
make PG_CONFIG=/path/to/patched/postgres/bin/pg_config
make PG_CONFIG=/path/to/patched/postgres/bin/pg_config install
```

Create and load them in dependency order:

```sql
CREATE EXTENSION pg_batch_bridge;
CREATE EXTENSION pg_batch_tam;
CREATE EXTENSION pg_batch;
LOAD 'pg_batch_tam';
LOAD 'pg_batch';
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

The suite checks heap and BRIN batches, native Arrow consumption, lazy `Datum`
fallback, exact and pruning-only predicates, parameters, rescans, early stop,
disabled-source fallback, ABI rejection, and duplicate provider rejection.

## Source layout

- `bridge/include/bridge.h` defines the common versioned ABI.
- `bridge/include/arrow.h` defines the optional Arrow interface.
- `bridge/bridge.c` owns the registry and slot attachments.
- `nodes/planner.c` builds sequential and BRIN-backed custom plans.
- `nodes/slot.c` implements the custom slot and heap batch source.
- `nodes/scan.c`, `filter.c`, and `aggregate.c` implement the executor nodes.
- `tam/provider.c` classifies source predicates and manages scans.
- `tam/compressed.c` owns snapshots, native columns, group pruning, and source
  filtering.
- `tam/tableam.c` is the heap-compatible test table access method boundary.

## Benchmark

Create the heap data once, then run the desired comparison scripts:

```sh
psql -f benchmark/setup.sql
psql -f benchmark/run_heap_compare.sql
psql -f benchmark/run_compressed.sql
psql -f benchmark/run_groups.sql
psql -f benchmark/run_brin.sql
```

`run_heap_compare.sql` checks that the bridge refactoring does not slow the
existing heap path. `run_compressed.sql` compares the PostgreSQL executor, heap
batches, and the independent native source. `run_groups.sql` compares full
heap scans, heap batches, BRIN, batch-over-BRIN, native batches, group pruning,
and exact source filtering. Current measurements and query explanations are in
[`benchmark/results.md`](benchmark/results.md).
