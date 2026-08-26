# Benchmark results

## What was measured

The original measurements were taken on 2026-08-24 with PostgreSQL master
`e42f1d0f1be` plus the heap deformation cursor required by this prototype. The
heap and BRIN comparison was added on 2026-08-25 using the same build and
database, and the storage-group measurements were refreshed on 2026-08-26.
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
| Ordered heap, 1000-row range | 32.536 | 14.647 | 0.265 | 0.116 |
| Ordered heap, medium range and sum | 39.533 | 22.349 | 18.323 | 10.314 |
| Wide heap, range, filter, and late sum | 8.040 | 5.188 | 2.544 | 1.083 |
| Ordered heap, impossible range | 30.522 | 12.415 | 0.040 | 0.024 |
| Uncorrelated heap, forced BRIN range | 15.794 | 6.588 | 23.023 | 9.807 |

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
source batches and evaluates the comparison in dense loops, reducing 0.265 ms
to 0.116 ms. Most of the improvement over sequential scanning comes from BRIN;
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
filtering and `PgBatchAgg` reduce executor overhead. Batch BRIN is 1.78 times
faster than Standard BRIN and 2.17 times faster than Batch Seq.

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
1.083 ms versus 2.544 ms for Standard BRIN.

### Query 4: a range that cannot match

```sql
SELECT count(*)
FROM pg_batch_bench_narrow
WHERE c1 < 0;
```

BRIN proves from its summaries that no heap page can match. Both bitmap plans
finish without opening a heap page. The difference between 0.040 and 0.024 ms
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

## Heap-path sanity check after adding source requests

A separate longer run disabled the compressed source and bitmap scans, then
compared only the current sequential heap batch path with the ordinary
executor. It used five warm-ups, 31 alternating samples, and ten executions
per sample.

| Query | Heap batch, ms | PostgreSQL, ms |
|---|---:|---:|
| Narrow: two comparisons and `count(*)` | 18.389 | 43.177 |
| Narrow: `c1 < 0` and `count(*)` | 11.017 | 28.956 |
| Wide: filter `c2`, return `c60` | 3.883 | 5.759 |
| Wide: filter `c60`, return `c2` | 4.105 | 11.350 |
| Wide: filter `c2`, batch `count(*)` | 3.912 | 8.068 |

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
- The BRIN batch path is serial and heap-specific. It deliberately reads
  `HeapScanDesc` page state as a playground shortcut; a production extension
  would need a supported core interface.
- These are warm-cache, serial, single-machine measurements. They validate the
  mechanism; they are not a general throughput claim for cold I/O or concurrent
  workloads.
