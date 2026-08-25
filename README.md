# pg_batch

`pg_batch` is a small executor playground built against PostgreSQL master. It
tests whether custom tuple slots and custom plan nodes are sufficient to pass
heap and columnar data between executor nodes in batches. PostgreSQL core gains
only a low-level heap deformation cursor; the slot ABI and table access method
API do not change.

The prototype provides three custom nodes:

- `PgBatchScan` exposes batches of up to 64 visible tuples from a pinned heap
  page and evaluates a safe prefix of simple restrictions in dense loops. It
  can visit every page sequentially or only pages selected by a BRIN bitmap.
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

For ordinary heap tables, the planner also recognizes core `BitmapHeapPath`
alternatives whose bitmap is built only from BRIN indexes. The custom path
keeps PostgreSQL's `Bitmap Index Scan`, uses the normal heap bitmap callback to
prune pages and check MVCC visibility, and then exposes the callback's
`rs_vistuples` array as batches. All relation restrictions are still checked
by the batch nodes because BRIN results are normally lossy. This experiment is
heap-specific and intentionally changes neither the table access method API
nor PostgreSQL core.

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
heap-compatible tables without dropped or missing columns. The bitmap batch
path is restricted to the built-in heap table access method and BRIN indexes.
The compressed source accepts only `int4` columns and uses `PLAIN`, `DELTA8`,
or `DELTA16` independently for each 64-row column. `PLAIN` becomes an Arrow
values buffer without copying; delta columns are decoded lazily into an Arrow
`int32` buffer, not a `Datum` array. The slot keeps its 64-bit row selection
separately; Arrow validity bits describe NULLs only. Unsupported queries keep
their ordinary PostgreSQL plans.

The extension also registers `pg_batch_compressed`, a test table access method.
Its physical storage and all ordinary callbacks are heap callbacks. Only a
scan into `PgBatchSlot` reads the compressed snapshot, including the request
for restrictions and lazy columns carried by that slot. Set
`pg_batch.compressed_via_tableam = on` to exercise this path. This isolates the
API experiment: the direct path and the table access method path share the
same storage groups, filtering code, and batch format.

## Source layout

- `pg_batch.c` contains module initialization and the extension GUC.
- `pg_batch_plan.c` adds sequential and BRIN-backed custom paths and builds the
  three custom plans.
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
psql -f benchmark/run_brin.sql
```

`benchmark/run_groups.sql` compares ordinary and batched heap scans, ordinary
and batched BRIN scans, and all compressed-source modes on two tables with the
same logical rows. It exercises BRIN min/max and Bloom operator classes. This
is the direct comparison used to decide whether BRIN is enough or a native
analytic table AM provides a separate benefit.

`benchmark/run_heap_compare.sql` uses longer alternating samples for checking
that changes to the format-neutral slot code do not slow down the heap source.
It disables bitmap scans so the BRIN indexes cannot change that comparison.

It covers dense simple restrictions, a residual scalar fallback, both
physical orders of filter and projection columns, an aggregate that needs no
projection Datum values, and BRIN page rejection over correlated and
uncorrelated heap data. The latest local measurements are recorded in
[`benchmark/results.md`](benchmark/results.md).
