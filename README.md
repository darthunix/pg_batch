# pg_batch

`pg_batch` is a small executor playground built against PostgreSQL master. It
tests whether custom tuple slots and custom plan nodes are sufficient to pass
heap and columnar data between executor nodes in batches. PostgreSQL core gains
only a low-level heap deformation cursor; the slot ABI and table access method
API do not change.

The prototype provides three custom nodes:

- `PgBatchScan` exposes batches of up to 64 visible tuples from a pinned heap
  page and evaluates a safe prefix of simple restrictions in dense loops.
- `PgBatchFilterProject` evaluates remaining qualifications, keeps a 64-bit
  selection mask, and materializes projection columns only for surviving
  rows.
- `PgBatchAgg` consumes complete batches for `count(*)`, `count(column)`, and
  `sum(int4)` without converting the batch to a row stream.

The same nodes can also read a backend-local compressed snapshot made by
`pg_batch_compress()`. Requested compressed columns are exposed as standard
Arrow C Data Interface `ArrowArray` objects. Batch filters and aggregates read
the Arrow value and validity buffers directly. PostgreSQL `Datum` values are
formed only when a residual expression or a row-at-a-time parent needs them.

The snapshot now groups 64-row executor batches into larger storage groups.
Each group stores per-column min/max values, NULL presence, and an exact small
set of values until that set overflows. `pg_batch.compressed_scan_mode` selects
plain batch reading, group pruning, or pruning plus exact source filtering.
The default group contains 4096 rows; `pg_batch_compress(regclass, integer)`
accepts another multiple of 64.

Source filtering currently understands the six built-in `int4` comparisons.
Other safe conditions remain in `PgBatchScan`. The small set is exact metadata,
not a Bloom or Fuse filter; it deliberately keeps this API experiment simple.

The slot stores only columns referenced by the query. Filter columns precede
projection-only columns in its compact descriptor. Each source row keeps a
PostgreSQL `HeapTupleDeformState`, which records the physical attribute
high-water mark and byte offset. Filter columns are extracted in column-major
loops. A later lazy projection continues the saved cursor only for surviving
rows; an earlier projection restarts only those rows.

The prototype build of PostgreSQL exposes `heap_deform_tuple_advance()`. It
extracts one requested physical attribute without forming Datums for the
intermediate attributes. Cached fixed offsets allow a direct jump, while
variable-length and nullable prefixes use PostgreSQL's compact attribute
metadata and deformation primitives. No scratch tuple slot or local crossover
rule is needed.

## Build

The required PostgreSQL change is included as
`patches/postgres/0001-Expose-incremental-heap-tuple-deformation.patch`.
Apply it to a PostgreSQL master checkout before building the extension:

```sh
git -C ../postgres am \
    ../pg_batch/patches/postgres/0001-Expose-incremental-heap-tuple-deformation.patch
```

Build `pg_batch` against that PostgreSQL installation by selecting its
`pg_config`:

```sh
make PG_CONFIG=/path/to/patched/postgres/bin/pg_config
make PG_CONFIG=/path/to/patched/postgres/bin/pg_config install
```

Load the library in each session that should use the planner hooks:

```sql
CREATE EXTENSION pg_batch;
LOAD 'pg_batch';
```

Alternatively, add `pg_batch` to `shared_preload_libraries`. Custom paths are
enabled by default after loading the library. Use `SET pg_batch.enable = off`
to compare a query with the ordinary executor.

To exercise the columnar source, make an in-memory snapshot and enable it:

```sql
SELECT pg_batch_compress('int4_table');
SET pg_batch.use_compressed = on;
SET pg_batch.compressed_scan_mode = filter;
```

The function returns the compressed size in bytes. The snapshot belongs to the
current backend and replaces an older snapshot of the same table. It is not
kept in sync with the source table, so recreate it after any data change.

The first version intentionally supports only serial forward scans of ordinary
heap-compatible tables without dropped or missing columns. The compressed
source accepts only `int4` columns and uses `PLAIN`, `DELTA8`, or `DELTA16`
independently for each 64-row column. `PLAIN` becomes an Arrow values buffer
without copying; delta columns are decoded lazily into an Arrow `int32` buffer,
not a `Datum` array. The slot keeps its 64-bit row selection separately; Arrow
validity bits describe NULLs only. Unsupported queries keep their ordinary
PostgreSQL plans.

The extension also registers `pg_batch_compressed`, a test table access method.
Its physical storage and all ordinary callbacks are heap callbacks. Only a
scan into `PgBatchSlot` reads the compressed snapshot, including the request
for restrictions and lazy columns carried by that slot. Set
`pg_batch.compressed_via_tableam = on` to exercise this path. This isolates the
API experiment: the direct path and the table access method path share the
same storage groups, filtering code, and batch format.

## Source layout

- `pg_batch.c` contains module initialization and the extension GUC.
- `pg_batch_plan.c` adds custom paths and builds the three custom plans.
- `pg_batch_slot.c` owns the batch slot, selection mask, lazy columns, and
  heap deformation cursors.
- `pg_batch_compress.c` owns the compressed snapshots and their lazy Arrow
  column views, storage-group metadata, pruning, and source filtering.
- `pg_batch_tableam.c` contains the heap-compatible test table access method.
- `pg_batch_scan.c`, `pg_batch_filter.c`, and `pg_batch_agg.c` implement
  the three executor nodes.
- `pg_batch_exec.c` contains the small amount of shared executor plumbing.
- `pg_batch.h` is the internal contract between these modules.

## Benchmark

The reproducible benchmark compares the same queries with `pg_batch.enable`
enabled and disabled:

```sh
psql -f benchmark/setup.sql
psql -f benchmark/run.sql
psql -f benchmark/run_mixed.sql
psql -f benchmark/run_compressed.sql
psql -f benchmark/run_groups.sql
```

`benchmark/run_heap_compare.sql` uses longer alternating samples for checking
that changes to the format-neutral slot code do not slow down the heap source.

It covers dense simple restrictions, a residual scalar fallback, both
physical orders of filter and projection columns, and an aggregate that needs
no projection Datum values. The latest local measurements are recorded in
[`benchmark/results.md`](benchmark/results.md).
