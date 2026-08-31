CREATE EXTENSION pg_batch_api;
CREATE EXTENSION pg_batch_tam;
CREATE EXTENSION pg_batch;
CREATE EXTENSION pg_batch_limit;

\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX
\set bridge_test :libdir '/pg_batch_bridge_test' :dlsuffix
\set kernels_test :libdir '/pg_batch_kernels_test' :dlsuffix
\set expr_test :libdir '/pg_batch_expr_test' :dlsuffix
\set runtime_test :libdir '/pg_batch_runtime_test' :dlsuffix
\set spill_test :libdir '/pg_batch_spill_test' :dlsuffix
LOAD :'bridge_test';
LOAD :'kernels_test';
LOAD :'expr_test';
LOAD :'runtime_test';
LOAD :'spill_test';
LOAD 'pg_batch_tam';
LOAD 'pg_batch';

CREATE FUNCTION pg_batch_bridge_test_bad_abi()
RETURNS void
AS :'bridge_test', 'pg_batch_bridge_test_bad_abi'
LANGUAGE C;
CREATE FUNCTION pg_batch_bridge_test_duplicate_provider()
RETURNS void
AS :'bridge_test', 'pg_batch_bridge_test_duplicate_provider'
LANGUAGE C;
CREATE FUNCTION pg_batch_bridge_test_compatible_sizes()
RETURNS boolean
AS :'bridge_test', 'pg_batch_bridge_test_compatible_sizes'
LANGUAGE C;
CREATE FUNCTION pg_batch_kernels_test()
RETURNS boolean
AS :'kernels_test', 'pg_batch_kernels_test'
LANGUAGE C;
CREATE FUNCTION pg_batch_expr_api_test()
RETURNS boolean
AS :'expr_test', 'pg_batch_expr_api_test'
LANGUAGE C;
CREATE FUNCTION pg_batch_datum_buffer_test()
RETURNS boolean
AS :'runtime_test', 'pg_batch_datum_buffer_test'
LANGUAGE C;
CREATE FUNCTION pg_batch_input_api_test()
RETURNS boolean
AS :'runtime_test', 'pg_batch_input_api_test'
LANGUAGE C;
CREATE FUNCTION pg_batch_output_test()
RETURNS boolean
AS :'runtime_test', 'pg_batch_output_test'
LANGUAGE C;
CREATE FUNCTION pg_batch_output_over_max_test()
RETURNS void
AS :'runtime_test', 'pg_batch_output_over_max_test'
LANGUAGE C;
CREATE FUNCTION pg_batch_output_double_finish_test()
RETURNS void
AS :'runtime_test', 'pg_batch_output_double_finish_test'
LANGUAGE C;
CREATE FUNCTION pg_batch_output_select_after_finish_test()
RETURNS void
AS :'runtime_test', 'pg_batch_output_select_after_finish_test'
LANGUAGE C;
CREATE FUNCTION pg_batch_output_fast_select_after_finish_test()
RETURNS void
AS :'runtime_test', 'pg_batch_output_fast_select_after_finish_test'
LANGUAGE C;
CREATE FUNCTION pg_batch_binding_request_test()
RETURNS boolean
AS :'runtime_test', 'pg_batch_binding_request_test'
LANGUAGE C;
CREATE FUNCTION pg_batch_request_after_seal_test()
RETURNS void
AS :'runtime_test', 'pg_batch_request_after_seal_test'
LANGUAGE C;
CREATE FUNCTION pg_batch_spill_test()
RETURNS boolean
AS :'spill_test', 'pg_batch_spill_test'
LANGUAGE C;

SELECT pg_batch_bridge_test_bad_abi();
SELECT pg_batch_bridge_test_duplicate_provider();
SELECT pg_batch_bridge_test_compatible_sizes() AS compatible_abi_sizes_ok;
SELECT pg_batch_kernels_test() AS installed_kernels_ok;
SELECT pg_batch_expr_api_test() AS installed_expr_api_ok;
SELECT pg_batch_datum_buffer_test() AS installed_datum_buffer_ok;
SELECT pg_batch_input_api_test() AS installed_input_api_ok;
SELECT pg_batch_output_test() AS installed_output_ok;
SELECT pg_batch_output_over_max_test();
SELECT pg_batch_binding_request_test() AS installed_binding_request_ok;
SELECT pg_batch_request_after_seal_test();
SELECT pg_batch_output_double_finish_test();
SELECT pg_batch_output_select_after_finish_test();
SELECT pg_batch_output_fast_select_after_finish_test();
SELECT pg_batch_spill_test() AS installed_spill_ok;

SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE pg_batch_test AS
SELECT g AS c1,
       repeat('x', g % 17) AS padding,
       CASE WHEN g % 13 = 0 THEN NULL ELSE g + 2 END AS c2,
       g + 3 AS c3,
       g + 4 AS c4,
       g + 5 AS c5,
       repeat('y', g % 19) AS padding2,
       g::bigint AS padding3,
       CASE WHEN g % 11 = 0 THEN NULL ELSE repeat('z', g % 23) END AS padding4,
       g + 6 AS c6
FROM generate_series(1, 80) AS g;

ANALYZE pg_batch_test;

CREATE TEMP TABLE pg_batch_kernel_values AS
SELECT CASE
           WHEN g = 1 THEN '-2147483648'::integer
           WHEN g = 2 THEN '2147483647'::integer
           WHEN g % 17 = 0 THEN NULL
           ELSE g - 66
       END AS v
FROM generate_series(1, 130) AS g;
ANALYZE pg_batch_kernel_values;

CREATE TEMP TABLE pg_batch_kernel_nulls(v integer);
INSERT INTO pg_batch_kernel_nulls
SELECT NULL::integer FROM generate_series(1, 70);
ANALYZE pg_batch_kernel_nulls;

SET pg_batch.enable = on;
SET pg_batch.enable_simd = on;
CREATE TEMP TABLE pg_batch_kernel_simd AS
SELECT (SELECT count(*) FROM pg_batch_kernel_values WHERE v = 0) AS eq,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE v <> 0) AS ne,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE v < 0) AS lt,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE v <= 0) AS le,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE v > 0) AS gt,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE 0 <= v) AS ge;

SET pg_batch.enable_simd = off;
CREATE TEMP TABLE pg_batch_kernel_scalar AS
SELECT (SELECT count(*) FROM pg_batch_kernel_values WHERE v = 0) AS eq,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE v <> 0) AS ne,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE v < 0) AS lt,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE v <= 0) AS le,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE v > 0) AS gt,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE 0 <= v) AS ge;

SET pg_batch.enable = off;
CREATE TEMP TABLE pg_batch_kernel_plain AS
SELECT (SELECT count(*) FROM pg_batch_kernel_values WHERE v = 0) AS eq,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE v <> 0) AS ne,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE v < 0) AS lt,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE v <= 0) AS le,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE v > 0) AS gt,
       (SELECT count(*) FROM pg_batch_kernel_values WHERE 0 <= v) AS ge;

SELECT NOT EXISTS (
           (TABLE pg_batch_kernel_simd
            EXCEPT ALL TABLE pg_batch_kernel_scalar)
           UNION ALL
           (TABLE pg_batch_kernel_scalar
            EXCEPT ALL TABLE pg_batch_kernel_simd)
       ) AS simd_matches,
       NOT EXISTS (
           (TABLE pg_batch_kernel_simd
            EXCEPT ALL TABLE pg_batch_kernel_plain)
           UNION ALL
           (TABLE pg_batch_kernel_plain
            EXCEPT ALL TABLE pg_batch_kernel_simd)
       ) AS postgres_matches;

SET pg_batch.enable = on;
SET pg_batch.enable_simd = on;
PREPARE pg_batch_kernel_parameter(integer) AS
SELECT count(*) FROM pg_batch_kernel_values WHERE v < $1;
EXECUTE pg_batch_kernel_parameter(0);
EXECUTE pg_batch_kernel_parameter(NULL);
DEALLOCATE pg_batch_kernel_parameter;

CREATE FUNCTION pg_batch_test_eq(integer, integer)
RETURNS boolean
LANGUAGE plpgsql
IMMUTABLE STRICT LEAKPROOF
AS 'BEGIN RETURN $1 = $2; END';
CREATE OPERATOR === (
    LEFTARG = integer,
    RIGHTARG = integer,
    FUNCTION = pg_batch_test_eq
);
SELECT count(*) AS fallback_count
FROM pg_batch_kernel_values
WHERE v === 0;
DROP OPERATOR === (integer, integer);
DROP FUNCTION pg_batch_test_eq(integer, integer);

RESET pg_batch.enable_simd;

SET pg_batch.enable = on;
CREATE TEMP TABLE pg_batch_reduction_batch AS
SELECT 'mixed' AS test, count(*) AS n, count(v) AS nn, sum(v) AS total,
       min(v) AS minimum, max(v) AS maximum
FROM pg_batch_kernel_values
UNION ALL
SELECT 'empty', count(*), count(v), sum(v), min(v), max(v)
FROM pg_batch_kernel_values WHERE v > '2147483647'::integer
UNION ALL
SELECT 'nulls', count(*), count(v), sum(v), min(v), max(v)
FROM pg_batch_kernel_nulls;

EXPLAIN (COSTS OFF)
SELECT count(*), count(v), sum(v), min(v), max(v)
FROM pg_batch_kernel_values;

SET pg_batch.enable = off;
CREATE TEMP TABLE pg_batch_reduction_plain AS
SELECT 'mixed' AS test, count(*) AS n, count(v) AS nn, sum(v) AS total,
       min(v) AS minimum, max(v) AS maximum
FROM pg_batch_kernel_values
UNION ALL
SELECT 'empty', count(*), count(v), sum(v), min(v), max(v)
FROM pg_batch_kernel_values WHERE v > '2147483647'::integer
UNION ALL
SELECT 'nulls', count(*), count(v), sum(v), min(v), max(v)
FROM pg_batch_kernel_nulls;

SELECT NOT EXISTS (
           (TABLE pg_batch_reduction_batch
            EXCEPT ALL TABLE pg_batch_reduction_plain)
           UNION ALL
           (TABLE pg_batch_reduction_plain
            EXCEPT ALL TABLE pg_batch_reduction_batch)
       ) AS reductions_match;

SET pg_batch.enable = on;
EXPLAIN (COSTS OFF)
SELECT sum(c6 + 1), count(c6 + 1), min(c6 + 1), max(c6 + 1)
FROM pg_batch_test
WHERE (c2 + 3) * 2 > 20;

CREATE TEMP TABLE pg_batch_expr_batch AS
SELECT sum(c6 + 1) AS plus_value,
       sum(100 - c6) AS left_value,
       sum(c6 * 2) AS multiplied,
       sum(c6 / 2) AS divided,
       sum(c6 % 7) AS modulo,
       sum(-c6) AS negated,
       count(c2 + 1) AS nonnull
FROM pg_batch_test
WHERE (c2 + 3) * 2 > 20;

PREPARE pg_batch_expr_parameter(integer, integer) AS
SELECT sum(c6 + $1) FROM pg_batch_test WHERE c2 + $1 < $2;
EXECUTE pg_batch_expr_parameter(3, 20);
EXECUTE pg_batch_expr_parameter(NULL, 20);
DEALLOCATE pg_batch_expr_parameter;

SET pg_batch.enable = off;
CREATE TEMP TABLE pg_batch_expr_plain AS
SELECT sum(c6 + 1) AS plus_value,
       sum(100 - c6) AS left_value,
       sum(c6 * 2) AS multiplied,
       sum(c6 / 2) AS divided,
       sum(c6 % 7) AS modulo,
       sum(-c6) AS negated,
       count(c2 + 1) AS nonnull
FROM pg_batch_test
WHERE (c2 + 3) * 2 > 20;

SELECT NOT EXISTS (
           (TABLE pg_batch_expr_batch EXCEPT ALL TABLE pg_batch_expr_plain)
           UNION ALL
           (TABLE pg_batch_expr_plain EXCEPT ALL TABLE pg_batch_expr_batch)
       ) AS expressions_match;

SET pg_batch.enable = on;
SELECT sum(c6 + 2147483647) FROM pg_batch_test;
SELECT count(*) FROM pg_batch_test WHERE 10 / (c1 - 1) > 0;
SELECT sum(-v) FROM pg_batch_kernel_values;

SET pg_batch.enable = on;
EXPLAIN (COSTS OFF)
SELECT avg(v) FROM pg_batch_kernel_values;
EXPLAIN (COSTS OFF)
SELECT count(DISTINCT v) FROM pg_batch_kernel_values;
EXPLAIN (COSTS OFF)
SELECT count(v) FILTER (WHERE v > 0) FROM pg_batch_kernel_values;
EXPLAIN (COSTS OFF)
SELECT v, count(*) FROM pg_batch_kernel_values GROUP BY v;

-- Grouped aggregation keeps NULL semantics and spills raw rows through the
-- reusable partitioned spill module when the resident group table is full.
CREATE TEMP TABLE pg_batch_group_values AS
SELECT CASE WHEN g % 101 = 0 THEN NULL ELSE g % 5000 END AS k,
       CASE WHEN g % 17 = 0 THEN NULL ELSE g % 97 END AS v
FROM generate_series(1, 20000) AS g;
ANALYZE pg_batch_group_values;
SET work_mem = '64kB';
SET pg_batch.enable = on;
CREATE TEMP TABLE pg_batch_group_result AS
SELECT k, count(*) AS n, count(v) AS nn, sum(v) AS total,
       min(v) AS minimum, max(v) AS maximum
FROM pg_batch_group_values
GROUP BY k;
SET pg_batch.enable = off;
CREATE TEMP TABLE pg_batch_group_plain AS
SELECT k, count(*) AS n, count(v) AS nn, sum(v) AS total,
       min(v) AS minimum, max(v) AS maximum
FROM pg_batch_group_values
GROUP BY k;
SELECT NOT EXISTS (
           (TABLE pg_batch_group_result EXCEPT ALL TABLE pg_batch_group_plain)
           UNION ALL
           (TABLE pg_batch_group_plain EXCEPT ALL TABLE pg_batch_group_result)
       ) AS grouped_spill_matches;
SET pg_batch.enable = on;
CREATE TEMP TABLE pg_batch_group_hidden AS
SELECT count(*) AS n FROM pg_batch_group_values GROUP BY k;
SET pg_batch.enable = off;
CREATE TEMP TABLE pg_batch_group_hidden_plain AS
SELECT count(*) AS n FROM pg_batch_group_values GROUP BY k;
SELECT NOT EXISTS (
           (TABLE pg_batch_group_hidden
            EXCEPT ALL TABLE pg_batch_group_hidden_plain)
           UNION ALL
           (TABLE pg_batch_group_hidden_plain
            EXCEPT ALL TABLE pg_batch_group_hidden)
       ) AS hidden_group_key_matches;
SET pg_batch.enable = on;
EXPLAIN (COSTS OFF)
SELECT k, count(*), sum(v) FROM pg_batch_group_values GROUP BY k;
RESET work_mem;

EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT c6 FROM pg_batch_test WHERE c2 < 10;

EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT c2 FROM pg_batch_test WHERE c6 < 14;

EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT c1, c3, c6 FROM pg_batch_test WHERE c4 < 14;

EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) FROM pg_batch_test WHERE c2 < 10;

EXPLAIN (COSTS OFF)
SELECT c6 FROM pg_batch_test WHERE (c2 + c3) % 7 = 0;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM pg_batch_test WHERE (c2 + c3) % 7 = 0;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM pg_batch_test;

EXPLAIN (COSTS OFF)
SELECT c6 FROM pg_batch_test
WHERE c4 > 10 AND (c2 + c3) % 7 = 0;

SET pg_batch.enable = on;
CREATE TEMP TABLE batch_result AS
SELECT c1, c3, c6
FROM pg_batch_test
WHERE (c2 + c3) % 7 = 0 AND c4 > 10;

CREATE TEMP TABLE batch_agg AS
SELECT count(*) AS n, count(c2) AS nn, sum(c2) AS total,
       min(c2) AS minimum, max(c2) AS maximum,
       sum(c6) AS other_total, min(c6) AS other_minimum,
       max(c6) AS other_maximum
FROM pg_batch_test
WHERE c3 > 30;

SET pg_batch.enable = off;
CREATE TEMP TABLE plain_result AS
SELECT c1, c3, c6
FROM pg_batch_test
WHERE (c2 + c3) % 7 = 0 AND c4 > 10;

CREATE TEMP TABLE plain_agg AS
SELECT count(*) AS n, count(c2) AS nn, sum(c2) AS total,
       min(c2) AS minimum, max(c2) AS maximum,
       sum(c6) AS other_total, min(c6) AS other_minimum,
       max(c6) AS other_maximum
FROM pg_batch_test
WHERE c3 > 30;

SELECT NOT EXISTS (
           (TABLE batch_result EXCEPT ALL TABLE plain_result)
           UNION ALL
           (TABLE plain_result EXCEPT ALL TABLE batch_result)
       ) AS rows_match,
       NOT EXISTS (
           (TABLE batch_agg EXCEPT ALL TABLE plain_agg)
           UNION ALL
           (TABLE plain_agg EXCEPT ALL TABLE batch_agg)
       ) AS aggregates_match;

SET pg_batch.enable = on;
CREATE TEMP TABLE batch_varlena AS
SELECT c1, padding, padding4
FROM pg_batch_test
WHERE c2 < 30;

SET pg_batch.enable = off;
CREATE TEMP TABLE plain_varlena AS
SELECT c1, padding, padding4
FROM pg_batch_test
WHERE c2 < 30;

SELECT NOT EXISTS (
           (TABLE batch_varlena EXCEPT ALL TABLE plain_varlena)
           UNION ALL
           (TABLE plain_varlena EXCEPT ALL TABLE batch_varlena)
       ) AS varlena_rows_match;

CREATE TABLE pg_batch_guaranteed
(
    c1 integer NOT NULL,
    c2 integer NOT NULL,
    padding text,
    c4 bigint
);
INSERT INTO pg_batch_guaranteed
SELECT g, g + 1, repeat('g', g % 9), g + 3
FROM generate_series(1, 20) AS g;
ANALYZE pg_batch_guaranteed;

SET pg_batch.enable = on;
CREATE TEMP TABLE batch_guaranteed AS
SELECT c1, c4 FROM pg_batch_guaranteed WHERE c2 < 10;

SET pg_batch.enable = off;
CREATE TEMP TABLE plain_guaranteed AS
SELECT c1, c4 FROM pg_batch_guaranteed WHERE c2 < 10;

SELECT NOT EXISTS (
           (TABLE batch_guaranteed EXCEPT ALL TABLE plain_guaranteed)
           UNION ALL
           (TABLE plain_guaranteed EXCEPT ALL TABLE batch_guaranteed)
       ) AS guaranteed_rows_match;

DROP TABLE pg_batch_guaranteed;

CREATE TABLE pg_batch_missing(c1 integer);
INSERT INTO pg_batch_missing VALUES (1), (2);
ALTER TABLE pg_batch_missing ADD COLUMN c2 text;
ANALYZE pg_batch_missing;

SET pg_batch.enable = on;
CREATE TEMP TABLE batch_missing AS
SELECT c1, c2 FROM pg_batch_missing WHERE c1 > 0;

SET pg_batch.enable = off;
CREATE TEMP TABLE plain_missing AS
SELECT c1, c2 FROM pg_batch_missing WHERE c1 > 0;

SELECT NOT EXISTS (
           (TABLE batch_missing EXCEPT ALL TABLE plain_missing)
           UNION ALL
           (TABLE plain_missing EXCEPT ALL TABLE batch_missing)
       ) AS missing_rows_match;

DROP TABLE pg_batch_missing;

SET pg_batch.enable = on;
PREPARE pg_batch_parameter(integer) AS
SELECT count(*), sum(c6) FROM pg_batch_test WHERE c2 < $1;
EXECUTE pg_batch_parameter(10);
DEALLOCATE pg_batch_parameter;

SELECT count(*), sum(c2) FROM pg_batch_test WHERE false;

CREATE TABLE pg_batch_brin AS
SELECT g AS c1,
       CASE WHEN g % 17 = 0 THEN NULL ELSE g + 1 END AS c2,
       repeat('b', 80) AS padding,
       g + 3 AS c4
FROM generate_series(1, 4096) AS g;
CREATE INDEX pg_batch_brin_c1_idx ON pg_batch_brin
USING brin(c1) WITH (pages_per_range = 1);
VACUUM (ANALYZE) pg_batch_brin;

SET enable_seqscan = off;
SET pg_batch.enable = on;
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*), sum(c4)
FROM pg_batch_brin
WHERE c1 BETWEEN 2001 AND 2200 AND c2 < 2150;

CREATE TEMP TABLE batch_brin_rows AS
SELECT c1, c4
FROM pg_batch_brin
WHERE c1 BETWEEN 2001 AND 2200 AND c2 < 2150 AND c4 % 7 = 0;

SET pg_batch.enable = off;
CREATE TEMP TABLE plain_brin_rows AS
SELECT c1, c4
FROM pg_batch_brin
WHERE c1 BETWEEN 2001 AND 2200 AND c2 < 2150 AND c4 % 7 = 0;

SELECT NOT EXISTS (
           (TABLE batch_brin_rows EXCEPT ALL TABLE plain_brin_rows)
           UNION ALL
           (TABLE plain_brin_rows EXCEPT ALL TABLE batch_brin_rows)
       ) AS brin_rows_match;

SET pg_batch.enable = on;
CREATE TEMP TABLE limited_brin_row AS
SELECT c1 FROM pg_batch_brin
WHERE c1 BETWEEN 2001 AND 2200
LIMIT 1;
SELECT count(*) = 1 AS brin_early_stop FROM limited_brin_row;

SELECT count(*) AS empty_brin
FROM pg_batch_brin WHERE c1 BETWEEN -10 AND -1;

PREPARE pg_batch_brin_parameter(integer, integer) AS
SELECT count(*), sum(c4)
FROM pg_batch_brin
WHERE c1 BETWEEN $1 AND $2 AND c2 < $2;
EXECUTE pg_batch_brin_parameter(2001, 2050);
EXECUTE pg_batch_brin_parameter(3001, 3050);
DEALLOCATE pg_batch_brin_parameter;

PREPARE pg_batch_brin_gate(boolean) AS
SELECT count(*) FROM pg_batch_brin
WHERE $1 AND c1 BETWEEN 2001 AND 2050;
EXECUTE pg_batch_brin_gate(false);
EXECUTE pg_batch_brin_gate(true);
DEALLOCATE pg_batch_brin_gate;

RESET enable_seqscan;
DROP TABLE pg_batch_brin;

CREATE TABLE pg_batch_bitmap AS
SELECT g AS id,
       g % 100 AS c1,
       (g / 100) % 100 AS c2,
       CASE WHEN g % 17 = 0 THEN NULL ELSE g + 3 END AS c3,
       repeat('x', 80) AS padding
FROM generate_series(1, 20000) AS g;
CREATE INDEX pg_batch_bitmap_c1_idx ON pg_batch_bitmap(c1);
CREATE INDEX pg_batch_bitmap_c2_idx ON pg_batch_bitmap(c2);
VACUUM (ANALYZE) pg_batch_bitmap;

SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET pg_batch.enable = on;
RESET pg_batch.bitmap_min_rows_per_page;
EXPLAIN (COSTS OFF)
SELECT count(*), sum(c3) FROM pg_batch_bitmap WHERE c1 = 7;

EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*), sum(c3)
FROM pg_batch_bitmap
WHERE c1 BETWEEN 0 AND 19;

EXPLAIN (COSTS OFF)
SELECT count(*), sum(c3)
FROM pg_batch_bitmap
WHERE c1 BETWEEN 0 AND 19 AND (id + c3) % 7 = 0;

SET pg_batch.bitmap_min_rows_per_page = 0;
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*), sum(c3) FROM pg_batch_bitmap WHERE c1 = 7;

EXPLAIN (COSTS OFF)
SELECT count(*), sum(c3)
FROM pg_batch_bitmap
WHERE c1 BETWEEN 10 AND 14 AND c2 BETWEEN 20 AND 29;

EXPLAIN (COSTS OFF)
SELECT count(*), sum(c3)
FROM pg_batch_bitmap
WHERE c3 > 0 AND (c1 = 7 OR c2 = 11);

SET pg_batch.enable = off;
CREATE TEMP TABLE plain_btree_rows AS
SELECT id, c1, c3, padding
FROM pg_batch_bitmap
WHERE c1 = 7 AND c3 IS NOT NULL AND (id + c3) % 7 = 0;
CREATE TEMP TABLE plain_bitmap_and AS
SELECT count(*) AS n, sum(c3) AS total
FROM pg_batch_bitmap
WHERE c1 BETWEEN 10 AND 14 AND c2 BETWEEN 20 AND 29;
CREATE TEMP TABLE plain_bitmap_or AS
SELECT count(*) AS n, sum(c3) AS total
FROM pg_batch_bitmap
WHERE c3 > 0 AND (c1 = 7 OR c2 = 11) AND id % 11 = 0;

SET pg_batch.enable = on;
CREATE TEMP TABLE batch_btree_rows AS
SELECT id, c1, c3, padding
FROM pg_batch_bitmap
WHERE c1 = 7 AND c3 IS NOT NULL AND (id + c3) % 7 = 0;
CREATE TEMP TABLE batch_bitmap_and AS
SELECT count(*) AS n, sum(c3) AS total
FROM pg_batch_bitmap
WHERE c1 BETWEEN 10 AND 14 AND c2 BETWEEN 20 AND 29;
CREATE TEMP TABLE batch_bitmap_or AS
SELECT count(*) AS n, sum(c3) AS total
FROM pg_batch_bitmap
WHERE c3 > 0 AND (c1 = 7 OR c2 = 11) AND id % 11 = 0;

SELECT NOT EXISTS (
           (TABLE batch_btree_rows EXCEPT ALL TABLE plain_btree_rows)
           UNION ALL
           (TABLE plain_btree_rows EXCEPT ALL TABLE batch_btree_rows)
       ) AS btree_rows_match,
       NOT EXISTS (
           (TABLE batch_bitmap_and EXCEPT ALL TABLE plain_bitmap_and)
           UNION ALL
           (TABLE plain_bitmap_and EXCEPT ALL TABLE batch_bitmap_and)
       ) AS bitmap_and_match,
       NOT EXISTS (
           (TABLE batch_bitmap_or EXCEPT ALL TABLE plain_bitmap_or)
           UNION ALL
           (TABLE plain_bitmap_or EXCEPT ALL TABLE batch_bitmap_or)
       ) AS bitmap_or_match;

RESET enable_indexonlyscan;
RESET enable_indexscan;
RESET enable_seqscan;
RESET pg_batch.bitmap_min_rows_per_page;
DROP TABLE pg_batch_bitmap;

CREATE TABLE pg_batch_arrow(c1 int, c2 int, c3 int, c4 int)
USING pg_batch_compressed;
INSERT INTO pg_batch_arrow
SELECT g,
       CASE WHEN g % 7 = 0 THEN NULL ELSE g + 1 END AS c2,
       CASE WHEN g % 2 = 0
            THEN 2147483647 - g
            ELSE -2147483647 + g
       END AS c3,
       g * 1000
FROM generate_series(1, 130) AS g;
ANALYZE pg_batch_arrow;

SET pg_batch.enable = off;
CREATE TEMP TABLE plain_arrow_rows AS
SELECT c1, c3
FROM pg_batch_arrow
WHERE c2 < 100 AND (c1 + c3) % 5 = 0;
CREATE TEMP TABLE plain_arrow_agg AS
SELECT count(*) AS n, count(c2) AS nn, sum(c1) AS total,
       min(c1) AS minimum, max(c1) AS maximum,
       sum(c4) AS delta16_total, min(c4) AS delta16_minimum,
       max(c4) AS delta16_maximum
FROM pg_batch_arrow
WHERE c2 < 100;

SELECT pg_batch_compress('pg_batch_arrow') > 0 AS compressed;
SET pg_batch.enable = on;

EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*), count(c2), sum(c1), sum(c4)
FROM pg_batch_arrow
WHERE c2 < 100;

EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT c1, c3
FROM pg_batch_arrow
WHERE c2 < 100 AND (c1 + c3) % 5 = 0;

CREATE TEMP TABLE compressed_arrow_rows AS
SELECT c1, c3
FROM pg_batch_arrow
WHERE c2 < 100 AND (c1 + c3) % 5 = 0;
CREATE TEMP TABLE compressed_arrow_agg AS
SELECT count(*) AS n, count(c2) AS nn, sum(c1) AS total,
       min(c1) AS minimum, max(c1) AS maximum,
       sum(c4) AS delta16_total, min(c4) AS delta16_minimum,
       max(c4) AS delta16_maximum
FROM pg_batch_arrow
WHERE c2 < 100;

SELECT NOT EXISTS (
           (TABLE compressed_arrow_rows EXCEPT ALL TABLE plain_arrow_rows)
           UNION ALL
           (TABLE plain_arrow_rows EXCEPT ALL TABLE compressed_arrow_rows)
       ) AS arrow_rows_match,
       NOT EXISTS (
           (TABLE compressed_arrow_agg EXCEPT ALL TABLE plain_arrow_agg)
           UNION ALL
           (TABLE plain_arrow_agg EXCEPT ALL TABLE compressed_arrow_agg)
       ) AS arrow_aggregates_match;

SELECT pg_batch_compress('pg_batch_arrow') > 0 AS recompressed;

CREATE TABLE pg_batch_groups(c1 int, c2 int, c3 int)
USING pg_batch_compressed;
INSERT INTO pg_batch_groups
SELECT g, (g % 4) * 2, g + 1000
FROM generate_series(1, 384) AS g;
ANALYZE pg_batch_groups;
SELECT pg_batch_compress('pg_batch_groups', 128) > 0 AS grouped;

SET pg_batch_tam.scan_mode = prune;
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) FROM pg_batch_groups WHERE c1 >= 130 AND c1 <= 140;

EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) FROM pg_batch_groups WHERE c2 = 3;

SET pg_batch_tam.scan_mode = filter;
CREATE TEMP TABLE smart_direct AS
SELECT c1, c3 FROM pg_batch_groups WHERE c2 = 2 AND c1 > 100;

SET pg_batch.enable = off;
CREATE TEMP TABLE smart_plain AS
SELECT c1, c3 FROM pg_batch_groups WHERE c2 = 2 AND c1 > 100;

SELECT NOT EXISTS (
           (TABLE smart_direct EXCEPT ALL TABLE smart_plain)
           UNION ALL
           (TABLE smart_plain EXCEPT ALL TABLE smart_direct)
       ) AS smart_rows_match;

CREATE TABLE pg_batch_tam(c1 int, c2 int, c3 int)
USING pg_batch_compressed;
INSERT INTO pg_batch_tam
SELECT g, (g % 4) * 2, g + 1000 FROM generate_series(1, 384) AS g;
ANALYZE pg_batch_tam;

SELECT pg_batch_compress('pg_batch_tam', 128) > 0 AS tam_grouped;
SET pg_batch.enable = on;
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*), sum(c3) FROM pg_batch_tam WHERE c2 = 2 AND c1 > 100;

CREATE TEMP TABLE smart_tam AS
SELECT c1, c3 FROM pg_batch_tam WHERE c2 = 2 AND c1 > 100;

CREATE TEMP TABLE smart_tam_expr AS
SELECT sum(c3 + 1) AS total, min(c3 + 1) AS minimum
FROM pg_batch_tam
WHERE c2 + 1 = 3;

SET pg_batch.enable = off;
CREATE TEMP TABLE smart_tam_expr_plain AS
SELECT sum(c3 + 1) AS total, min(c3 + 1) AS minimum
FROM pg_batch_tam
WHERE c2 + 1 = 3;
SET pg_batch.enable = on;

PREPARE pg_batch_tam_parameter(integer, integer) AS
SELECT count(*), sum(c3)
FROM pg_batch_tam
WHERE c2 = $1 AND c1 > $2;
EXECUTE pg_batch_tam_parameter(2, 100);
EXECUTE pg_batch_tam_parameter(4, 200);
DEALLOCATE pg_batch_tam_parameter;

SELECT input.c2, scanned.n
FROM (VALUES (0), (2), (4)) AS input(c2)
CROSS JOIN LATERAL (
    SELECT count(*) AS n
    FROM pg_batch_tam
    WHERE pg_batch_tam.c2 = input.c2 AND c1 > 100
) AS scanned
ORDER BY input.c2;

SELECT count(*) = 1 AS tam_early_stop
FROM (
    SELECT c1 FROM pg_batch_tam WHERE c2 = 2 LIMIT 1
) AS limited;

SELECT NOT EXISTS (
           (TABLE smart_tam EXCEPT ALL TABLE smart_plain)
           UNION ALL
           (TABLE smart_plain EXCEPT ALL TABLE smart_tam)
       ) AS tam_rows_match,
       NOT EXISTS (
           (TABLE smart_tam_expr EXCEPT ALL TABLE smart_tam_expr_plain)
           UNION ALL
           (TABLE smart_tam_expr_plain EXCEPT ALL TABLE smart_tam_expr)
       ) AS tam_expressions_match;

SET pg_batch_tam.enable = off;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM pg_batch_tam WHERE c2 = 2;
RESET pg_batch_tam.enable;

CREATE TABLE pg_batch_join_outer(k1 int, k2 int, v int);
CREATE TABLE pg_batch_join_inner(k1 int, k2 int, v int)
USING pg_batch_compressed;
INSERT INTO pg_batch_join_outer
SELECT CASE WHEN g % 29 = 0 THEN NULL ELSE g % 40 END,
       g % 7, g
FROM generate_series(1, 130) AS g;
INSERT INTO pg_batch_join_inner
SELECT CASE WHEN g % 31 = 0 THEN NULL ELSE g % 40 END,
       g % 7, g * 3
FROM generate_series(1, 80) AS g;
INSERT INTO pg_batch_join_inner VALUES (1, 1, 999), (1, 1, 1000);
ANALYZE pg_batch_join_outer;
ANALYZE pg_batch_join_inner;

SET enable_mergejoin = off;
SET enable_nestloop = off;
SET pg_batch.enable = off;
CREATE TEMP TABLE plain_join_rows AS
SELECT o.v AS outer_v, i.v AS inner_v, o.v + i.v AS projected
FROM pg_batch_join_outer o
JOIN pg_batch_join_inner i
  ON o.k1 = i.k1 AND o.k2 = i.k2 AND o.v < i.v
WHERE o.v > 10;
CREATE TEMP TABLE plain_join_agg AS
SELECT count(*) AS n, count(i.v) AS nn,
       sum(o.v) AS outer_total, sum(i.v) AS inner_total
FROM pg_batch_join_outer o
JOIN pg_batch_join_inner i
  ON o.k1 = i.k1 AND o.k2 = i.k2
WHERE o.v > 10;

SELECT pg_batch_compress('pg_batch_join_inner') > 0 AS join_compressed;
SET pg_batch.enable = on;
EXPLAIN (COSTS OFF)
SELECT o.v, i.v
FROM pg_batch_join_outer o
JOIN pg_batch_join_inner i ON o.k1 = i.k1 AND o.k2 = i.k2
WHERE o.v > 10;

CREATE TEMP TABLE batch_join_rows AS
SELECT o.v AS outer_v, i.v AS inner_v, o.v + i.v AS projected
FROM pg_batch_join_outer o
JOIN pg_batch_join_inner i
  ON o.k1 = i.k1 AND o.k2 = i.k2 AND o.v < i.v
WHERE o.v > 10;
CREATE TEMP TABLE batch_join_agg AS
SELECT count(*) AS n, count(i.v) AS nn,
       sum(o.v) AS outer_total, sum(i.v) AS inner_total
FROM pg_batch_join_outer o
JOIN pg_batch_join_inner i
  ON o.k1 = i.k1 AND o.k2 = i.k2
WHERE o.v > 10;

SELECT NOT EXISTS (
           (TABLE batch_join_rows EXCEPT ALL TABLE plain_join_rows)
           UNION ALL
           (TABLE plain_join_rows EXCEPT ALL TABLE batch_join_rows)
       ) AS join_rows_match,
       NOT EXISTS (
           (TABLE batch_join_agg EXCEPT ALL TABLE plain_join_agg)
           UNION ALL
           (TABLE plain_join_agg EXCEPT ALL TABLE batch_join_agg)
       ) AS join_aggregates_match;

CREATE TEMP TABLE batch_join_three_keys AS
SELECT count(*) AS n, sum(o.v) AS outer_total, sum(i.v) AS inner_total
FROM pg_batch_join_outer o
JOIN pg_batch_join_inner i
  ON o.k1 = i.k1 AND o.k2 = i.k2 AND o.v = i.v;
SET pg_batch.enable = off;
CREATE TEMP TABLE plain_join_three_keys AS
SELECT count(*) AS n, sum(o.v) AS outer_total, sum(i.v) AS inner_total
FROM pg_batch_join_outer o
JOIN pg_batch_join_inner i
  ON o.k1 = i.k1 AND o.k2 = i.k2 AND o.v = i.v;
SELECT NOT EXISTS (
           (TABLE batch_join_three_keys EXCEPT ALL TABLE plain_join_three_keys)
           UNION ALL
           (TABLE plain_join_three_keys EXCEPT ALL TABLE batch_join_three_keys)
       ) AS join_three_keys_match;
SET pg_batch.enable = on;

-- Pack a scalar child on either side of a batch hash join.
EXPLAIN (COSTS OFF)
SELECT count(*), sum(o.v), sum(x.payload)
FROM pg_batch_join_outer o
JOIN (VALUES (1, 100), (2, 200), (3, 300)) x(k, payload)
  ON o.k1 = x.k
WHERE o.v > 10;

SELECT count(*) AS rows, sum(o.v) AS outer_total,
       sum(x.payload) AS packed_total
FROM pg_batch_join_outer o
JOIN (VALUES (1, 100), (2, 200), (3, 300)) x(k, payload)
  ON o.k1 = x.k
WHERE o.v > 10;

EXPLAIN (COSTS OFF)
SELECT count(*), sum(g.k), sum(i.v)
FROM generate_series(1, 130) g(k)
JOIN pg_batch_join_inner i ON i.k1 = g.k
WHERE i.v > 0;

SELECT count(*) AS rows, sum(g.k) AS packed_total,
       sum(i.v) AS inner_total
FROM generate_series(1, 130) g(k)
JOIN pg_batch_join_inner i ON i.k1 = g.k
WHERE i.v > 0;

-- Exercise nullable multi-column hashing with packed inputs and spill.
CREATE TABLE pg_batch_join_multi_outer(k1 int, k2 int, v int)
USING pg_batch_compressed;
CREATE TABLE pg_batch_join_multi_inner(k1 int, k2 int, v int)
USING pg_batch_compressed;
INSERT INTO pg_batch_join_multi_outer
SELECT CASE WHEN g % 89 = 0 THEN NULL ELSE (g % 25000) + 1 END,
       CASE WHEN g % 97 = 0 THEN NULL ELSE ((g % 25000) + 1) % 17 END,
       g
FROM generate_series(1, 40000) AS g;
INSERT INTO pg_batch_join_multi_inner
SELECT CASE WHEN g % 101 = 0 THEN NULL ELSE g END,
       CASE WHEN g % 103 = 0 THEN NULL ELSE g % 17 END,
       g * 3
FROM generate_series(1, 20000) AS g;
ANALYZE pg_batch_join_multi_outer;
ANALYZE pg_batch_join_multi_inner;
SELECT pg_batch_compress('pg_batch_join_multi_outer') > 0
       AND pg_batch_compress('pg_batch_join_multi_inner') > 0
       AS multi_join_compressed;

SET work_mem = '64kB';
SET pg_batch.enable = on;
CREATE TEMP TABLE batch_join_multi_spill AS
SELECT count(*) AS n, sum(o.v) AS outer_total, sum(i.v) AS inner_total
FROM pg_batch_join_multi_outer o
JOIN pg_batch_join_multi_inner i
  ON o.k1 = i.k1 AND o.k2 = i.k2
WHERE o.v % 5 <> 0;
SELECT count(*) = 1 AS join_early_limit_ok
FROM (
    SELECT o.k1
    FROM pg_batch_join_multi_outer o
    JOIN pg_batch_join_multi_inner i
      ON o.k1 = i.k1 AND o.k2 = i.k2
    LIMIT 1
) limited;
SET pg_batch.enable = off;
CREATE TEMP TABLE plain_join_multi_spill AS
SELECT count(*) AS n, sum(o.v) AS outer_total, sum(i.v) AS inner_total
FROM pg_batch_join_multi_outer o
JOIN pg_batch_join_multi_inner i
  ON o.k1 = i.k1 AND o.k2 = i.k2
WHERE o.v % 5 <> 0;

SELECT NOT EXISTS (
           (TABLE batch_join_multi_spill EXCEPT ALL TABLE plain_join_multi_spill)
           UNION ALL
           (TABLE plain_join_multi_spill EXCEPT ALL TABLE batch_join_multi_spill)
       ) AS join_multi_spill_match;
RESET work_mem;

-- Use the join's hash function to exercise fallback and repartitioning skew.
CREATE TABLE pg_batch_join_spill_outer(k int, v int);
CREATE TABLE pg_batch_join_spill_inner(k int, v int);
CREATE FUNCTION pg_temp.pg_batch_test_murmur(v int4) RETURNS bigint
LANGUAGE plpgsql IMMUTABLE STRICT AS $$
DECLARE h bigint := v::bigint & 4294967295;
BEGIN
  h := (h # (h >> 16)) & 4294967295;
  h := ((h::numeric * 2246822507) % 4294967296)::bigint;
  h := (h # (h >> 13)) & 4294967295;
  h := ((h::numeric * 3266489909) % 4294967296)::bigint;
  RETURN (h # (h >> 16)) & 4294967295;
END
$$;
INSERT INTO pg_batch_join_spill_outer
SELECT g, g FROM generate_series(1, 100000) AS g
WHERE (pg_temp.pg_batch_test_murmur(g) & 7) = 0;
-- Rows absent from the build side exercise the complete spill key filter.
INSERT INTO pg_batch_join_spill_outer
SELECT g, g FROM generate_series(200001, 220000) AS g
WHERE (pg_temp.pg_batch_test_murmur(g) & 7) = 0;
INSERT INTO pg_batch_join_spill_inner
SELECT g, g * 3 FROM generate_series(1, 100000) AS g
WHERE (pg_temp.pg_batch_test_murmur(g) & 7) = 0;
ANALYZE pg_batch_join_spill_outer;
ANALYZE pg_batch_join_spill_inner;

SET work_mem = '64kB';
SET pg_batch.enable = on;
CREATE TEMP TABLE batch_join_spill AS
SELECT count(*) AS n, sum(o.v) AS outer_total, sum(i.v) AS inner_total
FROM pg_batch_join_spill_outer o
JOIN pg_batch_join_spill_inner i ON o.k = i.k
WHERE o.v > 0;
SET pg_batch.enable = off;
CREATE TEMP TABLE plain_join_spill AS
SELECT count(*) AS n, sum(o.v) AS outer_total, sum(i.v) AS inner_total
FROM pg_batch_join_spill_outer o
JOIN pg_batch_join_spill_inner i ON o.k = i.k
WHERE o.v > 0;

SELECT NOT EXISTS (
           (TABLE batch_join_spill EXCEPT ALL TABLE plain_join_spill)
           UNION ALL
           (TABLE plain_join_spill EXCEPT ALL TABLE batch_join_spill)
       ) AS join_spill_match;

INSERT INTO pg_batch_join_spill_outer
SELECT g, g FROM generate_series(100001, 150000) AS g
WHERE (pg_temp.pg_batch_test_murmur(g) & 7) = 0;
INSERT INTO pg_batch_join_spill_inner
SELECT g, g * 3 FROM generate_series(100001, 150000) AS g
WHERE (pg_temp.pg_batch_test_murmur(g) & 7) = 0;
ANALYZE pg_batch_join_spill_outer;
ANALYZE pg_batch_join_spill_inner;

SET pg_batch.enable = on;
CREATE TEMP TABLE batch_join_repartition AS
SELECT count(*) AS n, sum(o.v) AS outer_total, sum(i.v) AS inner_total
FROM pg_batch_join_spill_outer o
JOIN pg_batch_join_spill_inner i ON o.k = i.k
WHERE o.v > 0;
SET pg_batch.enable = off;
CREATE TEMP TABLE plain_join_repartition AS
SELECT count(*) AS n, sum(o.v) AS outer_total, sum(i.v) AS inner_total
FROM pg_batch_join_spill_outer o
JOIN pg_batch_join_spill_inner i ON o.k = i.k
WHERE o.v > 0;

SELECT NOT EXISTS (
           (TABLE batch_join_repartition EXCEPT ALL TABLE plain_join_repartition)
           UNION ALL
           (TABLE plain_join_repartition EXCEPT ALL TABLE batch_join_repartition)
       ) AS join_repartition_match;

RESET work_mem;

-- A separately built pass-through node uses only the public bridge/runtime.
LOAD 'pg_batch_limit';
SET pg_batch.enable = on;
SET pg_batch_limit.enable = on;
EXPLAIN (COSTS OFF)
SELECT c1, c6 FROM pg_batch_test WHERE c1 > 0 OFFSET 63 LIMIT 65;

PREPARE pg_batch_limit_query(bigint, bigint) AS
SELECT count(*) AS n, min(c1) AS first, max(c1) AS last
FROM (
    SELECT c1 FROM pg_batch_test WHERE c1 > 0 OFFSET $2 LIMIT $1
) limited;
EXECUTE pg_batch_limit_query(0, 0);
EXECUTE pg_batch_limit_query(1, 0);
EXECUTE pg_batch_limit_query(63, 0);
EXECUTE pg_batch_limit_query(64, 0);
EXECUTE pg_batch_limit_query(65, 0);
EXECUTE pg_batch_limit_query(65, 15);
EXECUTE pg_batch_limit_query(NULL, NULL);
EXECUTE pg_batch_limit_query(NULL, 64);
EXECUTE pg_batch_limit_query(9223372036854775807, 1);
EXECUTE pg_batch_limit_query(-1, 0);
EXECUTE pg_batch_limit_query(1, -1);
DEALLOCATE pg_batch_limit_query;

-- Correlated limits force rescans with new offset values.
SELECT boundary, count(*) AS n, min(c1) AS first, max(c1) AS last
FROM (VALUES (1), (63), (64), (65)) AS boundaries(boundary)
CROSS JOIN LATERAL (
    SELECT c1 FROM pg_batch_test WHERE c1 > 0
    OFFSET boundary - 1 LIMIT 1
) limited
GROUP BY boundary
ORDER BY boundary;

-- WITH TIES is deliberately left to PostgreSQL's ordinary Limit.
EXPLAIN (COSTS OFF)
SELECT c1 FROM pg_batch_test ORDER BY c1 FETCH FIRST 1 ROW WITH TIES;

SET pg_batch_limit.enable = off;
CREATE TEMP TABLE pg_batch_limit_tam_plain AS
SELECT c1, c3 FROM pg_batch_tam WHERE c2 IS NOT NULL OFFSET 17 LIMIT 65;
SET pg_batch_limit.enable = on;
CREATE TEMP TABLE pg_batch_limit_tam_batch AS
SELECT c1, c3 FROM pg_batch_tam WHERE c2 IS NOT NULL OFFSET 17 LIMIT 65;
SELECT NOT EXISTS (
           (TABLE pg_batch_limit_tam_batch
            EXCEPT ALL TABLE pg_batch_limit_tam_plain)
           UNION ALL
           (TABLE pg_batch_limit_tam_plain
            EXCEPT ALL TABLE pg_batch_limit_tam_batch)
       ) AS limit_tam_matches;

RESET enable_mergejoin;
RESET enable_nestloop;
DROP TABLE pg_batch_join_spill_inner;
DROP TABLE pg_batch_join_spill_outer;
DROP TABLE pg_batch_join_multi_inner;
DROP TABLE pg_batch_join_multi_outer;
DROP TABLE pg_batch_join_inner;
DROP TABLE pg_batch_join_outer;

DROP TABLE pg_batch_tam;
DROP TABLE pg_batch_groups;
DROP TABLE pg_batch_arrow;
DROP TABLE pg_batch_test;
DROP EXTENSION pg_batch_limit;
DROP EXTENSION pg_batch;
DROP EXTENSION pg_batch_tam;
DROP EXTENSION pg_batch_api;
