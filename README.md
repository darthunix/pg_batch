# pg_batch

`pg_batch` is a small executor playground built against PostgreSQL master. It
tests whether custom tuple slots and custom plan nodes are sufficient to pass
heap tuples between executor nodes in batches. PostgreSQL core gains only a
low-level heap deformation cursor; the slot ABI and table access method API do
not change.

The prototype provides three custom nodes:

- `PgBatchScan` exposes batches of up to 64 visible tuples from a pinned heap
  page and evaluates a safe prefix of simple restrictions in dense loops.
- `PgBatchFilterProject` evaluates remaining qualifications, keeps a 64-bit
  selection mask, and materializes projection columns only for surviving
  rows.
- `PgBatchAgg` consumes complete batches for `count(*)`, `count(column)`, and
  `sum(int4)` without converting the batch to a row stream.

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

The first version intentionally supports only serial forward scans of ordinary
heap tables without dropped or missing columns. Unsupported queries keep their
ordinary PostgreSQL plans.

## Source layout

- `pg_batch.c` contains module initialization and the extension GUC.
- `pg_batch_plan.c` adds custom paths and builds the three custom plans.
- `pg_batch_slot.c` owns the batch slot, selection mask, lazy columns, and
  heap deformation cursors.
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
```

It covers dense simple restrictions, a residual scalar fallback, both
physical orders of filter and projection columns, and an aggregate that needs
no projection Datum values. The latest local measurements are recorded in
[`benchmark/results.md`](benchmark/results.md).
