# Benchmark results

## Method

Measured on 2026-08-24 from PostgreSQL master `e42f1d0f1be` plus the heap
deformation cursor used by this prototype. Both sides used the same PostgreSQL
binary and database. Only `pg_batch.enable` changed, so the ordinary executor
also used the cursor patch's unchanged row-at-a-time deformation path.

Parallel query and JIT were disabled and caches were warm. Fixed-width results
are the median of three runs. Each run used one warm-up followed by five timed
executions in one backend. The narrow table contains 2,000,000 rows and eight
`int4` columns. The wide table contains 250,000 rows and sixty `int4` columns.

The mixed table contains 500,000 rows with sixteen alternating `int4`, `text`,
and `bigint` columns and regular NULL values. Its results are the median of 31
alternating executions after five warm-ups per executor.

## Fixed-width timings

| Test | pg_batch, ms | PostgreSQL, ms | Time change |
|---|---:|---:|---:|
| Narrow, two dense quals and `count(*)` | 21.157 | 45.181 | -53.2% |
| Narrow, pass-none qual and `count(*)` | 14.107 | 30.280 | -53.4% |
| Narrow, residual scalar fallback | 33.073 | 33.058 | +0.1% |
| Wide, early filter and late projection | 4.404 | 7.710 | -42.9% |
| Wide, late filter and early projection | 4.637 | 11.917 | -61.1% |
| Wide, `count(*)` without projection | 4.225 | 8.951 | -52.8% |

Negative values mean less execution time.

## Mixed-type timings

| Test | pg_batch, ms | PostgreSQL, ms | Time change |
|---|---:|---:|---:|
| Early filter and `count(*)` | 4.928 | 11.984 | -58.9% |
| Early filter and late projection | 4.803 | 9.026 | -46.8% |
| Late filter and `count(*)` | 11.758 | 19.599 | -40.0% |
| Late filter and early projection | 12.062 | 16.307 | -26.0% |

## Where the time goes

### Dense qualifiers and batch aggregation

The narrow query evaluates two supported comparisons in column-major loops.
`PgBatchAgg` then counts selected bits instead of receiving 1,499,898 rows and
running one ordinary aggregate transition per row. The pass-none query clears
every selection mask inside the scan and returns no rows to the aggregate.

For a residual-only condition such as `c1 % 1000 = 0`, the planner does not add
the batch path. Both measurements use the same ordinary `Seq Scan`, so the
0.1% difference is noise. A supported comparison after a residual expression
also stays in the scalar path to preserve planner order.

### Lazy projection

For `SELECT c60 ... WHERE c2 < 1000`, all 250,000 source rows provide `c2`, but
only 997 survivors provide `c60`. Their saved deformation cursors continue
from the filter high-water mark. The other 249,003 `c60` values are never
formed.

For `count(*)`, no projected Datum is needed at all. The aggregate consumes the
selection mask directly.

### Incremental heap deformation

`heap_deform_tuple_advance()` returns one requested physical attribute without
forming Datums for the prefix. On the fixed-width wide table, cached offsets
let the scan jump directly to `c60`. The 997 survivors that later request the
earlier `c2` restart from the tuple beginning, but that work is small compared
with materializing sixty attributes for every source row.

Variable-length and nullable columns prevent some direct offset jumps. The
cursor then walks the physical prefix with PostgreSQL's compact metadata and
deformation primitives, while the extension keeps the outer loop dense and
stores only requested Datums. This is why the mixed late-column cases improve
less than the fixed-width `c60` case, but still avoid the previous regression.

The earlier hybrid prototype used either a small local deformer or an ordinary
scratch tuple slot. On the same mixed table, the cursor version was 8--9%
faster for early columns and 25--27% faster for late columns. It also removes
the machine- and schema-specific seven-attribute crossover rule.

## Limits

The prototype materializes all requested filter columns before it evaluates
the dense qualifiers. It does not yet postpone a late filter column until an
earlier selective qualifier has run.

The custom path cost is intentionally reduced by 10% after basic usefulness
checks. A production cost model should account for physical layout, types,
nullability, expected survivors, and whether the parent consumes batches.

These are warm-cache, serial, single-machine measurements. They validate the
mechanism; they are not a general throughput claim for other storage engines,
concurrent workloads, or cold I/O.
