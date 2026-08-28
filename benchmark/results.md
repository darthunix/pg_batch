# Benchmark results

## What was measured

The original measurements were taken on 2026-08-24 with PostgreSQL master
`e42f1d0f1be` plus the heap deformation cursor required by this prototype. The
heap and BRIN comparison was added on 2026-08-25 using the same build and
database, and the storage-group measurements were refreshed on 2026-08-26.
The hash-join measurements were added on 2026-08-26 with the same release
build.
The Arrow IPC FDW measurements were added on 2026-08-27 with the same build
and database.
The B-tree bitmap, BRIN, and heap sanity measurements were refreshed later that
day after exact-page recheck elimination and the bitmap planner guard were
added. The B-tree script temporarily removed the BRIN indexes in a transaction
so every bitmap plan in that section was produced only by B-tree indexes.
All compared plans used the same PostgreSQL binary. The ordinary executor
therefore used the unchanged row-at-a-time path from that patched binary.

Parallel query and JIT were disabled. Relations and compressed snapshots were
already warm. Snapshot construction time was measured separately and was not
included in query execution time.

The report uses these executor names:

- **PostgreSQL**: the ordinary executor and a regular `Seq Scan`.
- **Heap batch**: `PgBatchScan`, `PgBatchFilterProject`, and, where supported,
  `PgBatchAgg` reading heap tuples in batches of 64.
- **PostgreSQL BRIN**: a core `Bitmap Index Scan` and row-at-a-time
  `Bitmap Heap Scan`.
- **Heap batch BRIN**: the same core bitmap producer followed by
  `PgBatchScan`, which exposes visible tuples from each selected heap page in
  batches.
- **Arrow batch**: the same batch nodes reading a backend-local compressed
  snapshot through Arrow C Data Interface views.
- **Prune**: the Arrow source skips 4096-row storage groups using metadata, but
  `PgBatchScan` still evaluates conditions for rows in groups that remain.
- **Filter**: the source both skips groups and evaluates supported `int4`
  comparisons before it returns a batch.
- **Heap batch hash join**: `PgBatchHashJoin` builds and probes native `int4`
  columns from heap batches and feeds `PgBatchAgg` without a scalar boundary.
- **Arrow batch hash join**: the same join and aggregate nodes consume the
  compressed source's Arrow views.

These comparisons answer different questions. PostgreSQL versus Heap batch
measures the executor changes over the same heap table. Heap batch versus
Arrow batch also changes the data representation and bypasses a normal heap
visibility scan. Arrow batch versus Prune isolates group skipping. Prune
versus Filter shows the effect of evaluating simple conditions in the source.

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

The split bridge build was also compared with the exact pre-refactor library
using the same PostgreSQL binary and heap tables. Current versus old medians
were 18.946 versus 18.681 ms for the dense query, 11.500 versus 11.266 ms for
the pass-none query, 3.995 versus 4.030 ms for wide `count(*)`, 4.121 versus
4.080 ms for an early filter and late projection, and 4.292 versus 4.137 ms
for the reverse column order. The changes range from -0.9% to +3.7%; the
largest absolute increase is 0.155 ms and this short query is also the noisiest
case. The bridge hash lookup no longer appears in a heap-path profile.

## Heap plus BRIN versus a compressed table AM

This experiment is intended to guide an architectural choice: whether an
analytic workload needs a new table access method, or whether an ordered heap
with BRIN and batch-aware executor nodes is sufficient.

Two tables contain the same 2,000,000 logical rows and the same five `int4`
columns. `c1` is physically ordered from 1 to 2,000,000. `small_set` contains
only `0, 2, 4, ..., 14`.

- `pg_batch_bench_heap_groups` is an ordinary heap table. It has a default
  min/max BRIN index on `c1` and an `int4_bloom_ops` BRIN index on `small_set`,
  both with `pages_per_range = 32`.
- `pg_batch_bench_tam` uses the heap-compatible test table AM. Its compressed
  snapshot contains 489 storage groups of 4096 rows and occupies 14,439,732
  bytes including metadata.

Each compressed group stores min/max values, NULL presence, and up to 16 exact
distinct values per column. The exact set is discarded after it overflows.
This is deliberately simpler than a production Bloom or Fuse filter.

Each timing is the median of 15 executions after three warm-ups. All seven
modes were rotated in one backend. PostgreSQL and Heap batch force sequential
heap access. PostgreSQL BRIN and Heap batch BRIN force bitmap access. The three
Arrow modes read the compressed snapshot. In the query text below, `source`
stands for the ordinary heap table in heap modes and for the test table-AM
table in Arrow modes.

| Query | PostgreSQL | Heap batch | PostgreSQL BRIN | Heap batch BRIN | Arrow batch | Prune | Filter |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1000-row range | 33.675 | 14.877 | 0.178 | 0.077 | 6.458 | 0.031 | 0.018 |
| Impossible range | 30.873 | 12.729 | 0.036 | 0.021 | 4.876 | 0.011 | 0.005 |
| Missing low-cardinality value | 32.901 | 11.912 | 0.168 | 0.150 | 4.839 | 0.016 | 0.006 |
| Dense filter and sum | 37.979 | 19.747 | 42.297 | 21.455 | 9.224 | 7.825 | 5.841 |

All times are milliseconds.

### Query 1: a selective range on the ordered column

```sql
SELECT count(*)
FROM source
WHERE c1 BETWEEN 1000001 AND 1001000;
```

The result is 1000. BRIN opens 32 heap pages and exposes 5,024 visible rows for
recheck. Heap batch BRIN processes those rows in 96 batches and takes 0.077 ms.
The compressed source rejects 488 of 489 groups, opens 4096 rows, and takes
0.018 ms after filtering inside the table AM.

The table AM is about four times faster, but the absolute difference is only
0.059 ms. Both approaches solve the important problem by avoiding almost the
whole table. This result alone is not a strong reason to build a new table AM.

### Query 2: a range that cannot match

```sql
SELECT count(*)
FROM source
WHERE c1 < 0;
```

BRIN and compressed min/max metadata both prove that the result is empty
without opening data pages or batches. Heap batch BRIN takes 0.021 ms and the
table-AM path takes 0.005 ms. These numbers mostly measure fixed executor
startup cost.

### Query 3: a missing value handled by BRIN Bloom

```sql
SELECT count(*)
FROM source
WHERE small_set = 7;
```

Every range has `minimum = 0` and `maximum = 14`, so the default BRIN min/max
operator class would not prove that 7 is absent. The `int4_bloom_ops` index
does: the bitmap is empty and neither BRIN plan opens a heap page. Heap batch
BRIN takes 0.150 ms.

The compressed group's exact set is `{0, 2, 4, 6, 8, 10, 12, 14}` and likewise
rejects all 489 groups without opening a batch. The table-AM path takes 0.006
ms. Its in-memory exact metadata is faster than scanning the BRIN Bloom index,
but both designs avoid the 2,000,000-row scan. This result shows why a fair
heap comparison must consider the appropriate BRIN operator class, not only
the default min/max summary.

### Query 4: dense filtering followed by a batch sum

```sql
SELECT sum(c4)
FROM source
WHERE c1 < 1500000
  AND small_set = 2;
```

The result uses 187,500 values. BRIN rejects the final quarter of the ordered
heap but still visits 9,568 pages and exposes 1,502,176 rows. At this
selectivity its bitmap overhead outweighs the pages it skips: Heap batch takes
19.747 ms and Heap batch BRIN takes 21.455 ms.

Arrow batch takes 9.224 ms even without group rejection. Pruning lowers that
to 7.825 ms, and exact source filtering lowers it to 5.841 ms. The roughly
3.4-fold difference from heap batching comes mainly from the compact native
column representation and direct filtering.

### Architectural conclusion

These results do not justify a new table AM merely to reject ordered time or
identifier ranges. For selective predicates correlated with physical heap
order, BRIN performs the coarse rejection and batch executor nodes efficiently
process the remaining pages. BRIN Bloom also handles the tested missing-value
case without reading heap pages. Before adding a new storage engine, it is
worth evaluating the built-in min/max, minmax-multi, and Bloom operator classes
or a type-specific BRIN operator class.

A native analytic format becomes useful for different reasons: compression,
column-oriented access, lazy decoding, and keeping values outside `Datum` form
across a batch subtree. It has a large advantage when a query still processes
a large fraction of the table. Those capabilities, rather than page rejection
alone, are the stronger justification for a separate table AM.

These timings show the value of the native representation and source
capabilities, but do not compare different ways to integrate that source. A
table AM would be needed if the representation must become persistent table
storage with normal PostgreSQL DDL, MVCC, maintenance, and indexing semantics;
a custom scan could be enough for a narrower experiment.

The compressed snapshot is backend-local, already in memory, and bypasses a
normal heap visibility scan. It is not a production persistent table AM, so
the absolute Arrow-versus-heap ratios should not be treated as a storage-engine
claim. The useful result is the boundary between the two designs:

- start with heap, BRIN, and format-neutral batch executor nodes for selective
  predicates on physically correlated data;
- consider a native format when compression, column-oriented access, or source
  operations not served well by BRIN are required; use a table AM only when
  that format also needs normal PostgreSQL storage integration, and validate
  it separately with persistent storage, MVCC, cold reads, updates, and
  concurrent workloads.

## Additional BRIN coverage

A separate run checks BRIN batching on the original eight-column narrow table,
the sixty-column wide table, and an uncorrelated table. It covers a wider range
of selectivities and verifies that lazy projection still works after BRIN has
selected the heap pages.

The three benchmark tables have a BRIN index on `c1` with
`pages_per_range = 32`. Each timing is the median of 15 executions after three
warm-ups. The four modes were rotated in one backend:

- **Standard Seq** uses PostgreSQL's regular `Seq Scan`.
- **Batch Seq** uses the existing sequential `PgBatchScan`.
- **Standard BRIN** uses PostgreSQL's `Bitmap Heap Scan`.
- **Batch BRIN** uses the same bitmap index plan and the new heap bitmap source
  in `PgBatchScan`.

| Query | Standard Seq, ms | Batch Seq, ms | Standard BRIN, ms | Batch BRIN, ms |
|---|---:|---:|---:|---:|
| Ordered heap, 1000-row range | 34.360 | 15.710 | 0.270 | 0.120 |
| Ordered heap, medium range and sum | 40.717 | 23.557 | 18.283 | 10.396 |
| Wide heap, range, filter, and late sum | 8.794 | 5.742 | 2.587 | 1.456 |
| Ordered heap, impossible range | 30.686 | 12.665 | 0.042 | 0.024 |
| Uncorrelated heap, forced BRIN range | 16.464 | 7.159 | 23.472 | 10.407 |

### Query 1: a narrow range in an ordered heap

```sql
SELECT count(*)
FROM pg_batch_bench_narrow
WHERE c1 BETWEEN 1000001 AND 1001000;
```

The result is 1000. The table contains 2,000,000 rows in 14,720 heap pages.
BRIN visits 64 pages containing 8,704 visible rows. Its range summary cannot
identify the exact 1000 rows, so both bitmap plans must check the condition
again. Standard BRIN performs that work one row at a time. Batch BRIN forms 192
source batches and evaluates the comparison in dense loops, reducing 0.270 ms
to 0.120 ms. Most of the improvement over sequential scanning comes from BRIN;
batching then removes part of the remaining per-row executor cost.

### Query 2: a medium range followed by a batch aggregate

```sql
SELECT sum(c4)
FROM pg_batch_bench_narrow
WHERE c1 BETWEEN 500001 AND 1000000
  AND c2 < 900000;
```

The two conditions leave 399,998 rows and the result is 279,999,599,995. BRIN
visits 3,712 pages containing 504,832 visible rows. This is large enough for
both kinds of work to matter: page rejection reduces the scan, while dense
filtering and `PgBatchAgg` reduce executor overhead. Batch BRIN is 1.76 times
faster than Standard BRIN and 2.27 times faster than Batch Seq.

### Query 3: filtering before a late projection on a wide heap

```sql
SELECT sum(c60)
FROM pg_batch_bench_wide
WHERE c1 BETWEEN 100001 AND 150000
  AND c2 < 125000;
```

The table has sixty `int4` columns. BRIN selects 1,696 of 8,384 pages and the
range contains 50,880 visible rows. The second condition leaves 24,998 rows.
Only those survivors need `c60`, so the query combines page rejection, dense
filtering, lazy late-column deformation, and a batch sum. Batch BRIN takes
1.456 ms versus 2.587 ms for Standard BRIN.

### Query 4: a range that cannot match

```sql
SELECT count(*)
FROM pg_batch_bench_narrow
WHERE c1 < 0;
```

BRIN proves from its summaries that no heap page can match. Both bitmap plans
finish without opening a heap page. The difference between 0.042 and 0.024 ms
is mostly fixed executor overhead and is too small for a useful speedup claim.
The important result is that adding the batch source does not force a heap
scan when BRIN rejects every range.

### Query 5: the boundary when physical order is absent

```sql
SELECT count(*)
FROM pg_batch_bench_plain
WHERE c1 BETWEEN 1000000 AND 1001000;
```

`c1` is pseudo-random and this particular range has no matches. The BRIN
summaries overlap the requested range almost everywhere, so the forced bitmap
plan still visits 5,152 of 5,406 pages and exposes 953,120 rows for recheck.
Standard BRIN is slower than Standard Seq, and Batch BRIN is slower than Batch
Seq. Batching reduces the cost of the bad bitmap plan, but it cannot repair an
index that rejects almost no blocks. The normal planner should choose a
sequential path in this case.

The ordered results support the intended composition: BRIN is responsible for
coarse page rejection, while `PgBatchScan`, lazy projection, and `PgBatchAgg`
remain responsible for efficient processing inside selected pages. No new
table access method or persistent columnar format is needed for this case.

## B-tree bitmap scans

This run checks heap-page batching with exact B-tree bitmaps. It was repeated
after adding exact-page recheck elimination and a conservative planner guard.
Each number is the median of 15 executions after three warm-ups. Six modes were
rotated in one backend, with JIT and parallel query disabled:

- **Standard Seq** and **Batch Seq** read the complete heap.
- **Standard Index** uses a row-at-a-time B-tree `Index Scan`.
- **Standard Bitmap** uses PostgreSQL's `Bitmap Heap Scan`.
- **Automatic** enables `pg_batch` with the default minimum of eight estimated
  matching rows per heap page. The table says which plan was selected.
- **Forced Batch** sets `pg_batch.bitmap_min_rows_per_page = 0` and therefore
  measures the batch bitmap path even when the guard would reject it.

| Query | Standard Seq, ms | Batch Seq, ms | Standard Index, ms | Standard Bitmap, ms | Automatic, ms (plan) | Forced Batch, ms |
|---|---:|---:|---:|---:|---:|---:|
| Ordered heap, 0.01% | 35.076 | 16.287 | 0.037 | 0.024 | 0.016 (standard) | 0.015 |
| Ordered heap, 0.1% | 34.113 | 15.530 | 0.110 | 0.091 | 0.081 (standard) | 0.056 |
| Ordered heap, 1% | 33.194 | 14.816 | 0.857 | 0.736 | 0.727 (standard) | 0.459 |
| Ordered heap, 10% | 34.646 | 15.548 | 8.183 | 7.594 | 4.906 (batch) | 4.838 |
| Ordered heap, 50% | 45.799 | 21.836 | 40.658 | 38.687 | 25.480 (batch) | 25.089 |
| Random heap, 0.01% | 17.968 | 8.416 | 0.044 | 0.030 | 0.021 (standard) | 0.025 |
| Random heap, 0.1% | 16.916 | 7.430 | 0.251 | 0.197 | 0.167 (standard) | 0.194 |
| Random heap, 1% | 17.102 | 7.789 | 2.244 | 1.569 | 1.462 (standard) | 1.507 |
| Random heap, 2% | 18.124 | 8.095 | 4.934 | 2.447 | 2.428 (standard) | 2.279 |
| Random heap, 3% | 17.873 | 8.389 | 7.530 | 3.425 | 2.992 (standard) | 2.915 |
| Random heap, 5% | 17.979 | 8.513 | 11.623 | 4.751 | 3.928 (batch) | 3.665 |
| Random heap, 7.5% | 18.204 | 8.614 | 18.049 | 6.107 | 5.435 (batch) | 5.210 |
| Random heap, 10% | 18.537 | 8.860 | 23.904 | 8.132 | 6.635 (batch) | 6.654 |
| Random heap, 50% | 22.741 | 11.341 | 113.630 | 24.404 | 17.389 (batch) | 17.304 |
| Random heap, two-index `BitmapAnd` | 19.254 | 10.969 | 24.156 | 4.091 | 3.739 (standard) | 3.580 |
| Wide heap, residual filter, late projection | 8.930 | 7.355 | 2.293 | 2.386 | 2.291 (standard) | 3.394 |

The very small timings are sensitive to fixed measurement noise. The plan
choice, rather than the sub-0.1 ms difference, is the useful result in those
rows.

### Exact pages and the occupancy boundary

The range query reads `c1`, counts its rows, and sums `c4`:

```sql
SELECT count(*), sum(c4)
FROM pg_batch_bench_plain
WHERE c1 BETWEEN 0 AND 107374182;
```

The B-tree has already proved both range conditions on an exact bitmap page.
`PgBatchScan` now skips those checks and does not materialize `c1` as a filter
Datum; at 5% the plan reports zero filter Datums and 99,998 skipped condition
evaluations for 49,999 rows. The forced batch path is no slower than Standard
Bitmap at the sparse 0.1% and 1% points, where the old implementation was 25%
and 12% slower. It is 23% faster at 5%, 18% faster at 10%, and 29% faster at
50% in this run.

For the random heap, actual occupancy rises from about 5.5 rows per page at 3%
to 9.2 at 5%. The default threshold of eight therefore leaves 3% and below on
the ordinary bitmap path and selects batching at 5% and above. For the ordered
heap the generic page estimate does not know the physical correlation, so the
guard is deliberately conservative through 1%; forced batching shows that a
future correlation-aware estimate could select it earlier.

### BitmapAnd and a scalar residual

The two-index query selects about 10% through each B-tree index:

```sql
SELECT count(*), sum(c4)
FROM pg_batch_bench_plain
WHERE c1 BETWEEN 0 AND 214748364
  AND c2 BETWEEN 0 AND 214748364;
```

The benchmark raises only the planning value of `cpu_operator_cost` while
preparing this query so both ordinary and batch cases use `BitmapAnd`; query
execution itself is unchanged. The intersection leaves 10,034 rows on 635
pages. The forced batch path takes 3.580 ms versus 4.091 ms for the ordinary
bitmap executor and skips 40,136 redundant condition evaluations. The default
guard remains conservative because PostgreSQL estimates a much lower page
occupancy for this correlated intersection.

The wide-table query combines an exact B-tree range, a scalar residual
expression, and a projection of the sixtieth column:

```sql
SELECT sum(c60)
FROM pg_batch_bench_wide
WHERE c1 BETWEEN 100001 AND 150000
  AND c2 % 7 = 0;
```

The exact range checks are skipped, but the residual expression still uses the
scalar fallback over 50,000 rows and 1,667 small batches. Forced batching is
42% slower here. The automatic guard detects this unsupported dense-loop shape
and keeps the ordinary `Bitmap Heap Scan`, which takes 2.291 ms versus 2.386
ms for Standard Bitmap in this run.

The result is a safe initial policy: exact bitmap support is generic, forced
mode remains available for experiments, and the default path is used only
when estimated occupancy is high enough and every restriction can run in a
dense column loop.

## Heap-path sanity check after adding source requests

A separate longer run disabled the compressed source and bitmap scans, then
compared only the current sequential heap batch path with the ordinary
executor. It used five warm-ups, 31 alternating samples, and ten executions
per sample.

| Query | Heap batch, ms | PostgreSQL, ms |
|---|---:|---:|
| Narrow: two comparisons and `count(*)` | 20.110 | 44.989 |
| Narrow: `c1 < 0` and `count(*)` | 12.113 | 30.258 |
| Wide: filter `c2`, return `c60` | 4.714 | 6.484 |
| Wide: filter `c60`, return `c2` | 5.065 | 11.916 |
| Wide: filter `c2`, batch `count(*)` | 4.699 | 10.359 |

This is a sanity check, not a direct before-and-after comparison with the old
library. It did not reveal an obvious loss in the heap path after the source
request was added.

## Batch `int4` hash join

This experiment uses a 1,000,000-row probe table and a 200,000-row build
table. The build key is unique and every probe key has one match. Three
in-memory queries separate lookup, output gathering, and an extra join
condition. The main query gathers both payloads and removes the first 100
probe rows:

```sql
SELECT count(*), sum(p.v), sum(b.v)
FROM pg_batch_join_probe p
JOIN pg_batch_join_build b ON p.k = b.k
WHERE p.v > 100;
```

The count-only query projects no payload columns. The residual query adds
`p.v < b.v` to the join condition, so the batch node evaluates the direct
`int4` comparison over candidate rows before aggregation.

The PostgreSQL plans use a serial core Hash Join followed by Aggregate. The
batch plans use `PgBatchScan`, `PgBatchHashJoin`, and `PgBatchAgg`. The Arrow
case changes only both sources to backend-local compressed snapshots. Each
number below is the median of 11 alternating executions after three warm-ups.
The benchmark sets `pg_batch.enable` before every execution and plans the
query in that mode.

| Query / memory mode | Executor | Median, ms | PostgreSQL time |
|---|---|---:|---:|
| Count only, 64 MB | PostgreSQL | 61.302 | 100.0% |
| Count only, 64 MB | Heap batch | 15.274 | 24.9% |
| Count only, 64 MB | Arrow batch | 10.391 | 17.0% |
| Payload filter, 64 MB | PostgreSQL | 72.469 | 100.0% |
| Payload filter, 64 MB | Heap batch | 26.121 | 36.0% |
| Payload filter, 64 MB | Arrow batch | 18.129 | 25.0% |
| Join residual, 64 MB | PostgreSQL | 64.492 | 100.0% |
| Join residual, 64 MB | Heap batch | 25.514 | 39.6% |
| Join residual, 64 MB | Arrow batch | 15.210 | 23.6% |
| Payload filter, 1 MB spill | PostgreSQL | 117.939 | 100.0% |
| Payload filter, 1 MB spill | Heap batch | 56.797 | 48.2% |
| Payload filter, 1 MB spill | Arrow batch | 44.790 | 38.0% |

The distinct-key hash table uses open addressing, with chains only for true
duplicate keys. The common unique-key case fuses lookup with result-pair
creation. This makes heap batching about 2.5x to 4.0x faster than the core
plan in the in-memory cases above. Native Arrow inputs also avoid heap
deformation. Spill is about 2.0x faster here.

The Arrow spill case uses the same private spill representation and join code
as the heap case. Its remaining advantage comes before spill: the source
already exposes native columns, while the heap source must deform tuples into
columns first.

The spill path now keeps build partitions in memory while it writes the
others. Probe rows for those resident partitions are joined during the first
probe pass. A compact filter containing every build hash rejects keys that
cannot match before projection-only columns are materialized or written. The
filter uses one bit lookup per row: two lookups rejected slightly more rows but
made the all-match case unnecessarily expensive.

With 1 MB `work_mem`, PostgreSQL's hash memory limit is 2 MB. The batch join
selected eight partitions and kept one of them in memory. It retained 25,147
of 200,000 build rows and immediately joined 125,719 probe rows. Spill output
fell from 14.5 MB to 12.7 MB, and only 874,181 of 999,900 probe rows were
written and read again. Total memory reported for the join, including the
resident table, spill buffers, and the 128 kB key filter, peaked at 961 kB.

The filter is most useful when many probe keys are absent. Three additional
tables keep 50%, 10%, or none of their probe keys in the build key range. The
query still requests both payloads:

```sql
SELECT count(*), sum(p.v), sum(b.v)
FROM probe p
JOIN build b ON p.k = b.k;
```

Each result below is the median of 31 alternating executions after three
warm-ups:

| Probe keys present in build | PostgreSQL, ms | Heap batch, ms | PostgreSQL time |
|---:|---:|---:|---:|
| 100% | 111.497 | 55.129 | 49.4% |
| 50% | 102.830 | 45.481 | 44.2% |
| 10% | 89.371 | 36.576 | 40.9% |
| 0% | 82.990 | 32.040 | 38.6% |

In the zero-match run, the filter rejected 827,098 of 1,000,000 probe rows.
Only 151,283 rows were written to a probe file, and the heap source formed the
payload column for 172,902 rows rather than for the whole input. The difference
between those counts is the resident portion, which also needs its payload but
does not touch a temporary file.

An internal measurement switch, removed after the experiment, isolated the
filter cost from the resident path:

| Probe keys present in build | Filter off, ms | Filter on, ms | Time change |
|---:|---:|---:|---:|
| 100% | 54.901 | 55.611 | +1.3% |
| 50% | 57.373 | 45.130 | -21.3% |
| 10% | 53.281 | 35.999 | -32.4% |
| 0% | 51.425 | 30.832 | -40.0% |

The resident optimization was also measured separately before adding the key
filter. It reduced the regular 1 MB spill case from 57.731 to 55.349 ms, a
4.1% improvement. The gain is intentionally smaller than the 12.6% reduction
in probe spill because hashing and result production remain unchanged.

More memory lets a larger fraction of the build stay resident:

| `work_mem` | PostgreSQL, ms | Heap batch, ms | Resident probe fraction |
|---:|---:|---:|---:|
| 1 MB | 117.939 | 56.797 | 12.6% |
| 2 MB | 111.645 | 55.880 | about 25% |
| 4 MB | 102.683 | 55.701 | 49.9% |

The 4 MB plan used two partitions and avoided temporary-file traffic for
499,360 probe rows. Its runtime changed less than its spill volume because the
query still hashes, joins, gathers, and aggregates every matching row.

### Spill page and build-format experiments

The executor batch remains 64 rows. A prototype independently enlarged spill
records according to the memory budget. It reduced the number of records from
about 16,000 to a few hundred, but `BufFile` already combines small writes. In
alternating measurements the larger records improved the all-match case from
55.708 to 55.048 ms and the no-match case from 52.884 to 52.090 ms, only
1.2--1.5%. This was below the 3% retention threshold, so the extra format and
reader state were removed.

Time Profiler samples were also taken for the two-column and seventeen-column
spill queries. `load_build_chunk()` accounted for about 6% and 8% of batch
join CPU respectively. The current spill file and `BuildStore` are both
column-oriented, so a separate build representation would mainly replace one
column copy with another. The profile did not meet the 10% threshold for that
experiment, and no second build format was added.

The wide query forces eight payload columns from each side:

```sql
SELECT count(*),
       sum(p.v1), ..., sum(p.v8),
       sum(b.v1), ..., sum(b.v8)
FROM probe_wide p
JOIN build_wide b ON p.k = b.k;
```

At 1 MB it took 188.715 ms in the batch executor and 231.023 ms in PostgreSQL,
so the batch path used 81.7% of PostgreSQL time. This case narrows the gain
because both executors must move sixteen payload columns through temporary
storage and aggregation.

### Larger spill

A separate run scales the same unique-key join to 10,000,000 probe rows and
2,000,000 build rows with 8 MB `work_mem`. Two warm-ups were followed by seven
alternating executions:

| Executor | Median, ms | PostgreSQL time |
|---|---:|---:|
| PostgreSQL | 1,244.659 | 100.0% |
| Heap batch | 596.725 | 47.9% |

The batch plan kept 249,733 build rows and joined 1,248,665 probe rows without
temporary files. It wrote 126.9 MB for the remaining rows and reported 7.1 MB
peak memory. This is still a warm-cache machine-local test, but it confirms
that resident routing remains bounded and useful at ten times the original
row count.

The spill writer first computes each selected row's hash and links rows into
per-partition lists. It then appends them to 64-row buffers, avoiding both a
scan of the input batch per partition and many short temporary-file records.
The disk format remains simple: block header, row hashes, then one validity
word and dense `int32` values for each column.

Two controlled-skew checks use keys whose three low `murmurhash32` bits are
zero, so they enter the same initial partition. With 64 kB `work_mem`, 12,595
build and probe rows need fewer than six build chunks; the join keeps the
multi-pass fallback because another write is more expensive. The 125,044-row
case would reread all probe rows for each chunk, so the join partitions both
files once more using higher hash bits. This reduced probe rows read from
2,876,012 to 125,044. Separate same-cluster measurements compare the previous
and current batch spill paths:

| Spill case | Previous, ms | Current, ms | Time change |
|---|---:|---:|---:|
| Regular 1 MB spill | 84.916 | 57.463 | -32.3% |
| 12,595 skewed rows, bounded multi-pass fallback | 1.795 | 1.723 | -4.0% |
| 125,044 skewed rows, secondary partitioning | 45.108 | 34.877 | -22.7% |

The six-pass threshold matters. Forcing the small case through another
partitioning level increased it from about 1.6 to 2.9 ms, while disabling
secondary partitioning increased the large case from about 34 to 40 ms. The
skew keys were selected using the prototype's `murmurhash32`; PostgreSQL's
Hash Join uses a different hash function and therefore does not see the same
skew. Its timing is not used as a baseline for these two stress checks.

## Arrow IPC FDW and the direct batch-provider boundary

This experiment checks whether an FDW can keep its normal `ForeignScan` path
and also return source-native batches directly to the independent batch nodes.
It uses the same PostgreSQL build and the existing narrow and wide tables. The
tables were exported once as uncompressed Arrow IPC Streams with 4,096 rows
per record batch. The narrow file was 64,239,064 bytes and the wide file was
60,187,520 bytes. Exporting 2,000,000 narrow rows took 121.7 ms; exporting
250,000 wide rows took 92.4 ms. Export time is not included below.

Every result is the median of 11 alternating executions after three warm-ups.
Parallel query, JIT, and index paths were disabled. Files and relations were
warm. The five modes are:

- **Heap**: ordinary PostgreSQL `Seq Scan` and scalar executor over the local
  heap table. This is a storage and executor reference, not the same source.
- **FDW row**: the same Arrow reader applies supported filters, then returns
  survivors one row at a time through a normal `ForeignScan` slot.
- **FDW batch, no push**: `PgBatchScan` receives every 64-row window and its
  dense loop applies the filter over native Arrow values.
- **FDW batch, push**: the FDW applies supported comparisons before publishing
  a window. `PgBatchAgg` consumes the surviving batches directly.
- **FDW batch, eager**: the previous mode, but every Arrow column is decoded
  for every record batch. This is a control for lazy column decoding.

The dense narrow query scans all 2,000,000 rows, keeps 1,499,898, and sums the
late `c8` column:

```sql
SELECT count(*), sum(c8)
FROM pg_batch_bench_narrow_fdw
WHERE c1 > 100 AND c2 < 1500000;
```

The pass-none query scans the same source but returns no rows:

```sql
SELECT count(*)
FROM pg_batch_bench_narrow_fdw
WHERE c1 < 0;
```

The wide queries each keep 997 of 250,000 rows. The first filters on early
`c2` and sums late `c60`; the second reverses those roles:

```sql
SELECT sum(c60) FROM pg_batch_bench_wide_fdw WHERE c2 < 1000;
SELECT sum(c2)  FROM pg_batch_bench_wide_fdw WHERE c60 < 1058;
```

| Query | Heap, ms | FDW row, ms | Batch no push, ms | Batch push, ms | Batch eager, ms |
|---|---:|---:|---:|---:|---:|
| Narrow: two filters, count and sum 1,499,898 rows | 60.771 | 76.644 | 12.707 | 10.172 | 10.787 |
| Narrow: filter rejects all rows | 34.017 | 5.983 | 7.173 | 5.119 | 5.976 |
| Wide: filter `c2`, sum `c60` for 997 rows | 8.907 | 3.399 | 2.698 | 2.424 | 3.416 |
| Wide: filter `c60`, sum `c2` for 997 rows | 13.380 | 3.217 | 2.647 | 2.471 | 3.480 |

The row FDW is slow on the dense query because 1.5 million survivors cross
the scalar `ForeignScan` boundary and its test fallback materializes all eight
columns for each row. Direct batches avoid that boundary, keep Arrow `int32`
buffers through filtering and aggregation, and form no Datum values for these
queries. Moving a supported filter into the source saves another 9--20% here
because empty windows are never published and selected windows arrive with
their final selection bitmap.

The wide lazy plan read all 60,187,520 bytes, because Arrow IPC Stream record
bodies are not column-addressable. It nevertheless decoded only 63 column
arrays: the filter column in all 62 record batches and the projection column
in the one record batch with survivors. Eager mode decoded 3,720 arrays. That
raised execution time from 2.424 to 3.416 ms for the early-filter query and
from 2.471 to 3.480 ms for the late-filter query. This validates separate
filter and projection requests even when they cannot reduce physical I/O.

With pushdown disabled, the same 3,907 windows reach `PgBatchScan`; with exact
pushdown only 16 nonempty windows are published. Both modes decode the same 63
arrays for the selective wide query. The remaining 0.2--0.3 ms difference is
the cost of crossing the source boundary and invoking the executor filter for
all windows, rather than a data-format conversion.

The result is the intended interface check: no FDW callback or PostgreSQL core
FDW API changed. One optional `next_batch` operation in the bridge lets an
independent FDW bypass scalar `ForeignScan` only when a batch-aware custom path
is selected. The normal FDW path remains usable by ordinary PostgreSQL nodes.

## Reusable int4 filter kernels and SIMD

This experiment moves common selection and column access into `runtime/` and
the six built-in `int4` comparisons into `kernels/`. The scalar kernel calls a
direct comparison instead of `FunctionCallInvoke()`. The optional SIMD kernel
uses NEON on this ARM64 machine and is selected only for a dense selection
word. Sparse words continue to visit only their selected rows.

The first comparison repeated `run_heap_compare.sql` before and after the
change. Each result is the median of 31 alternating samples, with ten query
executions per sample. "Previous" is committed version `f3e2d7a`. "Scalar"
uses the new direct kernel with `pg_batch.enable_simd = off`.

| Heap query | Previous, ms | Scalar, ms | SIMD, ms | SIMD versus previous |
|---|---:|---:|---:|---:|
| Two dense conditions | 20.314 | 18.219 | 15.225 | -25.1% |
| One condition rejects all rows | 11.749 | 10.993 | 9.468 | -19.4% |
| Wide count without projection | 4.024 | 3.813 | 3.791 | -5.8% |
| Early filter, late projection | 4.041 | 3.859 | 3.822 | -5.4% |
| Late filter, early projection | 4.247 | 4.053 | 3.994 | -6.0% |

The directory also contains `run_kernels.sql`, which alternates the ordinary
executor, the direct scalar kernel, and SIMD in one backend. It tests both the
heap Datum layout and packed `int32` values from the compressed source. Native
source filtering is disabled, so these comparisons run in `PgBatchScan`.

| Source and selection entering the kernels | Scalar, ms | SIMD, ms | Change |
|---|---:|---:|---:|
| Heap: one condition rejects all | 13.704 | 12.465 | -7.7% |
| Heap: two dense conditions | 18.103 | 15.356 | -15.4% |
| Heap: half survives the first condition | 16.769 | 14.592 | -13.3% |
| Heap: sparse random second condition | 7.866 | 7.229 | -9.5% |
| Packed `int32`: one condition rejects all | 4.841 | 3.299 | -27.5% |
| Packed `int32`: two dense conditions | 6.948 | 4.431 | -35.8% |
| Packed `int32`: sparse random second condition | 2.012 | 1.429 | -29.1% |

In the sparse cases the first condition still receives a full selection and
uses SIMD. The second condition receives scattered survivors and uses the
scalar set-bit loop. The larger packed-`int32` gain comes from loading four
values per vector without narrowing the 8-byte Datum representation. On the
wide heap queries, tuple deformation and projection dominate, so SIMD adds
only about 1% after the direct scalar comparison. No measured heap query was
slower than the committed version.

## Limits of the measurements

- The prototype prepares all requested filter columns before evaluating the
  dense conditions. It does not yet delay a late filter column until an earlier
  selective condition has run.
- Exact source filtering currently supports the six built-in `int4`
  comparisons. Unsupported conditions remain in `PgBatchScan`.
- The table access method is a heap-compatible interface test. The compressed
  snapshot is backend-local, already in memory, and is not maintained after
  table changes.
- The Arrow FDW accepts only local, uncompressed Arrow IPC Streams containing
  `int4` columns. It reads every record body before lazily decoding requested
  arrays, has a placeholder row estimate, and is not a general-purpose FDW.
- PostgreSQL versus Arrow timings include representation, visibility, and
  column-selection differences. They are not a pure measure of batching.
- The custom path cost is deliberately reduced by 10% after basic usefulness
  checks. A production cost model must account for layout, types, nullability,
  expected survivors, group pruning, and whether the parent consumes batches.
- The BRIN batch path is serial and heap-specific. It deliberately reads
  `HeapScanDesc` page state as a playground shortcut; a production extension
  would need a supported core interface.
- `PgBatchHashJoin` currently supports only serial, unparameterized inner
  joins with direct `int4 = int4` keys. Its path cost is only a placeholder,
  and its spill files are private to one executor node.
- These are warm-cache, serial, single-machine measurements. They validate the
  mechanism; they are not a general throughput claim for cold I/O or concurrent
  workloads.
