# Benchmark results

## What was measured

The measurements were taken on 2026-08-24 with PostgreSQL master
`e42f1d0f1be` plus the heap deformation cursor required by this prototype.
All compared plans used the same PostgreSQL binary and database. The ordinary
executor therefore used the unchanged row-at-a-time path from that patched
binary.

Parallel query and JIT were disabled. Relations and compressed snapshots were
already warm. Snapshot construction time was measured separately and was not
included in query execution time.

The report uses these executor names:

- **PostgreSQL**: the ordinary executor and a regular `Seq Scan`.
- **Heap batch**: `PgBatchScan`, `PgBatchFilterProject`, and, where supported,
  `PgBatchAgg` reading heap tuples in batches of 64.
- **Arrow batch**: the same batch nodes reading a backend-local compressed
  snapshot through Arrow C Data Interface views.
- **Prune**: the Arrow source skips 4096-row storage groups using metadata, but
  `PgBatchScan` still evaluates conditions for rows in groups that remain.
- **Filter**: the source both skips groups and evaluates supported `int4`
  comparisons before it returns a batch.
- **TAM filter**: the same reader and filtering code as **Filter**, reached
  through `table_scan_getnextslot()` of the test table access method.

These comparisons answer different questions. PostgreSQL versus Heap batch
measures the executor changes over the same heap table. Heap batch versus
Arrow batch also changes the data representation and bypasses a normal heap
visibility scan. Batch versus Prune isolates group skipping. Prune versus
Filter shows the effect of evaluating simple conditions in the source. Filter
versus TAM filter isolates the cost of going through the table access method.

## Test data

### Narrow fixed-width table

`pg_batch_bench_narrow` contains 2,000,000 rows and eight `int4` columns:

```sql
SELECT g AS c1,
       g + 1 AS c2,
       ...,
       g + 7 AS c8
FROM generate_series(1, 2000000) AS g;
```

Rows are physically ordered by `g`. All columns change by one between adjacent
rows, which makes the table easy to delta-compress.

### Wide fixed-width table

`pg_batch_bench_wide` contains 250,000 rows and sixty `int4` columns. Column
`ci` contains `g + i`, so `c2 = g + 2` and `c60 = g + 60`.

The table is used to measure lazy access to columns that are physically far
apart. It distinguishes the cost of filtering on an early column and later
reading a late column from the reverse order.

### Mixed-width table

`pg_batch_bench_mixed` contains 500,000 rows and sixteen columns. `int4`,
`text`, and `bigint` columns alternate, and several `text` and `bigint` columns
contain regular NULL values. The first column `a` and the last column `p` both
contain `g`.

This layout prevents a deformer from treating the tuple as a simple fixed-size
array. Reaching `p` may require walking over variable-length and nullable
attributes.

### PLAIN-heavy table

`pg_batch_bench_plain` contains 1,000,000 rows and four pseudo-random `int4`
columns. `c1` normally uses the `PLAIN` encoding because adjacent differences
are large. Most batches of `c4` use `DELTA16` because its usual adjacent
difference is 16,807.

## Heap batch execution over fixed-width tables

For these measurements, one warm-up was followed by five executions in one
backend. The table reports the median of three such runs.

### Exact queries

The dense narrow query is:

```sql
SELECT count(*)
FROM pg_batch_bench_narrow
WHERE c1 > 100
  AND c2 < 1500000;
```

It scans all 2,000,000 rows and keeps 1,499,898. Both conditions are simple
comparisons supported by the dense batch loop. `PgBatchAgg` counts selected
bits instead of receiving 1,499,898 scalar rows.

The pass-none query is:

```sql
SELECT count(*)
FROM pg_batch_bench_narrow
WHERE c1 < 0;
```

It scans the full table but returns no rows. It measures the cost of loading
one filter column, applying one comparison, and clearing every selection mask.

The scalar fallback query is:

```sql
SELECT c8
FROM pg_batch_bench_narrow
WHERE c1 % 1000 = 0;
```

The left side is an expression rather than a simple column. The planner does
not add the batch path for a residual-only condition, so both measurements use
the same ordinary `Seq Scan`. The query returns 2,000 rows.

The wide-table queries are:

```sql
-- Filter on an early physical column and project a late one.
SELECT c60
FROM pg_batch_bench_wide
WHERE c2 < 1000;

-- Filter on a late physical column and project an early one.
SELECT c2
FROM pg_batch_bench_wide
WHERE c60 < 1058;

-- Filter on c2, but do not project a table column.
SELECT count(*)
FROM pg_batch_bench_wide
WHERE c2 < 1000;
```

Each condition keeps 997 of 250,000 rows. The first query should avoid reading
`c60` for 249,003 rejected rows. The second must read `c60` for every row, but
only 997 survivors need `c2`. The count query needs no projection Datum at all.

### Results

| Query | Heap batch, ms | PostgreSQL, ms | Time change |
|---|---:|---:|---:|
| Two comparisons, 1,499,898 rows counted | 21.157 | 45.181 | -53.2% |
| `c1 < 0`, no rows counted | 14.107 | 30.280 | -53.4% |
| `c1 % 1000 = 0`, ordinary scalar fallback | 33.073 | 33.058 | +0.1% |
| Filter `c2`, return `c60` for 997 rows | 4.404 | 7.710 | -42.9% |
| Filter `c60`, return `c2` for 997 rows | 4.637 | 11.917 | -61.1% |
| Filter `c2`, batch `count(*)` | 4.225 | 8.951 | -52.8% |

Negative time means that the batch path was faster. The fallback result is a
useful control: because both sides execute the same plan, the 0.1% difference
is measurement noise.

## Heap batch execution over mixed-width tuples

These results are the median of 31 alternating executions after five warm-ups
for each executor. Alternating the execution order reduces bias from changing
machine load.

The exact queries are:

```sql
-- Read and filter the first physical column. Every row passes.
SELECT count(*) FROM pg_batch_bench_mixed WHERE a > 0;

-- Reach the sixteenth physical column for every row. Every row passes.
SELECT count(*) FROM pg_batch_bench_mixed WHERE p > 0;

-- Filter on the first column, then read the last column for 999 survivors.
SELECT sum(p) FROM pg_batch_bench_mixed WHERE a < 1000;

-- Filter on the last column, then read the first column for 1,057 survivors.
SELECT sum(a) FROM pg_batch_bench_mixed WHERE p < 1058;
```

| Query | Heap batch, ms | PostgreSQL, ms | Time change |
|---|---:|---:|---:|
| Filter early `a`, count all 500,000 rows | 4.928 | 11.984 | -58.9% |
| Filter late `p`, count all 500,000 rows | 11.758 | 19.599 | -40.0% |
| Filter early `a`, sum late `p` for 999 rows | 4.803 | 9.026 | -46.8% |
| Filter late `p`, sum early `a` for 1,057 rows | 12.062 | 16.307 | -26.0% |

The late-column cases improve less because the deformer must walk over
variable-length and nullable columns before it reaches `p`. They still avoid
forming intermediate Datums and avoid projecting a column for rejected rows.

`heap_deform_tuple_advance()` extracts one requested physical attribute and
saves the high-water attribute number and byte offset for that tuple. A later
projection can continue from that cursor when its column is after the filter
column. If the projection column is before the saved cursor, only surviving
rows restart from the tuple beginning.

An earlier hybrid prototype selected either a small local deformer or an
ordinary scratch tuple slot. On this mixed table, the cursor version was 8--9%
faster for early columns and 25--27% faster for late columns. It also removed
the machine- and schema-specific seven-attribute crossover rule.

## Compressed Arrow source without storage-group pruning

`pg_batch_compress()` builds a backend-local compressed snapshot. Each 64-row
column batch independently uses `PLAIN`, `DELTA8`, or `DELTA16`. Requested
columns are exposed as Arrow C Data Interface arrays. `PLAIN` values are read
in place. Delta values are decoded lazily into an `int32` values buffer, not a
Datum array. The row selection mask remains separate from Arrow validity bits.

The `compressed_scan_mode` for these measurements was `batch`, so storage-group
metadata was ignored and every compressed batch was visited. This section
therefore measures the original compressed source rather than pruning.

### Snapshot size and build time

Logical size counts only uncompressed `int4` values and excludes heap tuple
headers. Compressed size includes the encoded batches and storage-group
metadata.

| Source | Logical size | Compressed size | Compressed/logical | Build time, ms |
|---|---:|---:|---:|---:|
| 2,000,000 rows × 8 mostly-delta columns | 61 MB | 22 MB | 35.6% | 106.133 |
| 250,000 rows × 60 mostly-delta columns | 57 MB | 20 MB | 35.0% | 67.736 |
| 1,000,000 rows × 4 PLAIN-heavy columns | 15 MB | 15 MB | 98.9% | 32.979 |

### Exact queries and results

The first five queries are the same narrow and wide queries described above.
The sixth replaces `count(*)` with a native batch sum:

```sql
SELECT sum(c60)
FROM pg_batch_bench_wide
WHERE c2 < 1000;
```

It filters all 250,000 rows using `c2` and reads `c60` only for the 997
survivors. `PgBatchAgg` consumes the Arrow `int32` buffer directly.

The PLAIN and DELTA16 tests are:

```sql
-- c1 is PLAIN-heavy. About half of the rows pass.
SELECT count(*)
FROM pg_batch_bench_plain
WHERE c1 < 1073741824;

-- The filter still reads PLAIN-heavy c1; the aggregate reads mostly-DELTA16 c4.
SELECT sum(c4)
FROM pg_batch_bench_plain
WHERE c1 < 1073741824;
```

| Query | PostgreSQL, ms | Heap batch, ms | Arrow batch, ms |
|---|---:|---:|---:|
| Narrow: two comparisons and `count(*)` | 45.536 | 22.950 | 7.639 |
| Narrow: `c1 < 0` and `count(*)` | 31.478 | 13.983 | 5.189 |
| Wide: filter `c2`, return `c60` for 997 rows | 7.487 | 4.407 | 1.021 |
| Wide: filter `c60`, return `c2` for 997 rows | 12.379 | 4.546 | 0.989 |
| Wide: filter `c2`, batch `count(*)` | 8.650 | 3.998 | 0.900 |
| Wide: filter `c2`, native batch `sum(c60)` | 6.942 | 3.773 | 0.801 |
| PLAIN `c1` filter and `count(*)` | 18.196 | 6.331 | 2.077 |
| PLAIN `c1` filter and mostly-DELTA16 `sum(c4)` | 18.542 | 8.964 | 2.957 |

PostgreSQL versus Arrow batch is not a pure executor comparison. The Arrow
source is an in-memory snapshot, reads only requested columns, and does not
repeat the normal heap visibility work. Heap batch versus Arrow batch is the
more useful comparison for deciding whether the same batch nodes can consume
another native representation without converting it to row Datums.

The simplified format dispatch was also compared with the exact pre-refactor
library using the same PostgreSQL binary and heap tables. A 20-second,
single-client `pgbench` run measured 3.956 ms versus 3.935 ms for the wide
`count(*)` query, a 0.5% increase. The dense narrow query measured 18.617 ms
versus 18.971 ms, so no slowdown was observed there. Short alternating samples
were noisier because other work on the machine changed even ordinary executor
timings.

## Storage-group pruning and filtering through a table AM

This experiment adds one low-cardinality column to the 2,000,000-row narrow
table and stores the physical heap table under the test table access method:

```sql
CREATE TABLE pg_batch_bench_tam
(
    c1 int,
    c2 int,
    c3 int,
    c4 int,
    small_set int
)
USING pg_batch_compressed;

INSERT INTO pg_batch_bench_tam
SELECT g,
       g + 1,
       g + 2,
       g + 3,
       (g % 8) * 2
FROM generate_series(1, 2000000) AS g;
```

`c1` is physically ordered from 1 to 2,000,000. `small_set` contains only
`0, 2, 4, ..., 14`. The compressed snapshot uses 4096-row storage groups, each
containing 64 executor batches of 64 rows. There are 489 groups and 31,250
batches in total. Its size including group metadata is 14,441,688 bytes.

Each group stores per-column min/max values, NULL presence, and up to 16 exact
distinct values. The exact set is discarded for pruning after it overflows.
It is intentionally a simple test structure, not a Bloom or Fuse filter.

Each timing below is the median of 15 executions after three warm-ups. The five
executor variants were rotated in one backend.

### Query 1: a 1000-row range inside the table

```sql
SELECT count(*)
FROM pg_batch_bench_tam
WHERE c1 BETWEEN 1000001 AND 1001000;
```

The result is 1000. Without group metadata, PostgreSQL and Batch inspect all
2,000,000 source rows. Min/max metadata rejects 488 of 489 groups. The one
remaining group contains 4096 rows in 64 batches. Source filtering keeps 1000
rows, and only 29,184 encoded bytes are touched.

| Executor | Time, ms | Source rows visited | Source batches opened |
|---|---:|---:|---:|
| PostgreSQL | 28.984 | 2,000,000 heap rows | n/a |
| Batch | 5.247 | 2,000,000 compressed rows | 31,250 |
| Prune | 0.021 | 4,096 compressed rows | 64 |
| Filter | 0.016 | 4,096 compressed rows | 64 |
| TAM filter | 0.019 | 4,096 compressed rows | 64 |

Most of the improvement comes from rejecting groups. Filter is slightly
cheaper than Prune because direct `int4` comparisons in the source replace the
generic operator calls for the remaining rows.

### Query 2: a range that cannot match

```sql
SELECT count(*)
FROM pg_batch_bench_tam
WHERE c1 < 0;
```

The result is zero. Every group has `minimum >= 1`, so min/max metadata rejects
all 489 groups without opening a data batch.

| Executor | Time, ms | Source rows visited | Source batches opened |
|---|---:|---:|---:|
| PostgreSQL | 27.282 | 2,000,000 heap rows | n/a |
| Batch | 3.924 | 2,000,000 compressed rows | 31,250 |
| Prune | 0.007 | 0 | 0 |
| Filter | 0.003 | 0 | 0 |
| TAM filter | 0.007 | 0 | 0 |

The difference between 0.003 and 0.007 ms is not meaningful. At this scale the
measurement is dominated by fixed executor startup cost. The useful result is
that no compressed batch was opened.

### Query 3: a missing value inside the min/max range

```sql
SELECT count(*)
FROM pg_batch_bench_tam
WHERE small_set = 7;
```

The result is zero. In every group, min/max is `[0, 14]`, so min/max alone
cannot reject 7. The exact set is `{0, 2, 4, 6, 8, 10, 12, 14}`, which proves
that 7 is absent and rejects all groups.

| Executor | Time, ms | Source rows visited | Source batches opened |
|---|---:|---:|---:|
| PostgreSQL | 30.483 | 2,000,000 heap rows | n/a |
| Batch | 3.941 | 2,000,000 compressed rows | 31,250 |
| Prune | 0.010 | 0 | 0 |
| Filter | 0.005 | 0 | 0 |
| TAM filter | 0.007 | 0 | 0 |

This query exists specifically to demonstrate metadata that can reject a group
when min/max cannot.

### Query 4: dense filtering followed by a native batch sum

```sql
SELECT sum(c4)
FROM pg_batch_bench_tam
WHERE c1 < 1500000
  AND small_set = 2;
```

The first condition keeps 1,499,999 rows. The second keeps one row out of every
eight, leaving 187,500 values for the aggregate. The result is 140,625,000,000.

Min/max metadata rejects only the final 122 groups. The source must still open
367 groups, or 23,488 batches containing 1,503,232 rows. In Filter mode, the
source applies both comparisons and `PgBatchAgg` reads `c4` from the native
batch only for surviving batches.

| Executor | Time, ms | Source rows visited | Source batches opened |
|---|---:|---:|---:|
| PostgreSQL | 35.874 | 2,000,000 heap rows | n/a |
| Batch | 7.717 | 2,000,000 compressed rows | 31,250 |
| Prune | 6.531 | 1,503,232 compressed rows | 23,488 |
| Filter | 5.937 | 1,503,232 compressed rows | 23,488 |
| TAM filter | 5.931 | 1,503,232 compressed rows | 23,488 |

This is the most useful table-AM comparison because enough data is processed
for fixed startup noise to be small. Direct Filter and TAM filter differ by
0.006 ms, which is inside normal run-to-run noise. Carrying the source request
in `PgBatchSlot` and entering the same reader through `table_scan_getnextslot()`
did not add a visible cost.

The test table access method is physically heap-compatible. Ordinary scans use
the heap callbacks unchanged. Only a scan into `PgBatchSlot` switches to the
backend-local compressed snapshot. This experiment therefore validates the
request and batch-delivery interface; it is not yet a persistent columnar table
access method.

## Heap-path sanity check after adding source requests

A separate longer run disabled the compressed source and compared only the
current heap batch path with the ordinary executor. It used five warm-ups,
31 alternating samples, and ten executions per sample.

| Query | Heap batch, ms | PostgreSQL, ms |
|---|---:|---:|
| Narrow: two comparisons and `count(*)` | 18.460 | 42.665 |
| Narrow: `c1 < 0` and `count(*)` | 11.285 | 28.725 |
| Wide: filter `c2`, return `c60` | 3.808 | 5.951 |
| Wide: filter `c60`, return `c2` | 4.037 | 11.342 |
| Wide: filter `c2`, batch `count(*)` | 3.810 | 8.454 |

This is a sanity check, not a direct before-and-after comparison with the old
library. It did not reveal an obvious loss in the heap path after the source
request was added.

## Limits of the measurements

- The prototype prepares all requested filter columns before evaluating the
  dense conditions. It does not yet delay a late filter column until an earlier
  selective condition has run.
- Exact source filtering currently supports the six built-in `int4`
  comparisons. Unsupported conditions remain in `PgBatchScan`.
- The table access method is a heap-compatible interface test. The compressed
  snapshot is backend-local, already in memory, and is not maintained after
  table changes.
- PostgreSQL versus Arrow timings include representation, visibility, and
  column-selection differences. They are not a pure measure of batching.
- The custom path cost is deliberately reduced by 10% after basic usefulness
  checks. A production cost model must account for layout, types, nullability,
  expected survivors, group pruning, and whether the parent consumes batches.
- These are warm-cache, serial, single-machine measurements. They validate the
  mechanism; they are not a general throughput claim for cold I/O or concurrent
  workloads.
