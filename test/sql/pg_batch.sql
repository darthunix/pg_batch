CREATE EXTENSION pg_batch_bridge;
CREATE EXTENSION pg_batch_tam;
CREATE EXTENSION pg_batch;

\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX
\set bridge_test :libdir '/pg_batch_bridge_test' :dlsuffix
LOAD :'bridge_test';
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

SELECT pg_batch_bridge_test_bad_abi();
SELECT pg_batch_bridge_test_duplicate_provider();

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
SELECT count(*) AS n, count(c6) AS nn, sum(c2) AS total
FROM pg_batch_test
WHERE c3 > 30;

SET pg_batch.enable = off;
CREATE TEMP TABLE plain_result AS
SELECT c1, c3, c6
FROM pg_batch_test
WHERE (c2 + c3) % 7 = 0 AND c4 > 10;

CREATE TEMP TABLE plain_agg AS
SELECT count(*) AS n, count(c6) AS nn, sum(c2) AS total
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
       sum(c4) AS delta16_total
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
       sum(c4) AS delta16_total
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
       ) AS tam_rows_match;

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
RESET enable_mergejoin;
RESET enable_nestloop;
DROP TABLE pg_batch_join_spill_inner;
DROP TABLE pg_batch_join_spill_outer;
DROP TABLE pg_batch_join_inner;
DROP TABLE pg_batch_join_outer;

DROP TABLE pg_batch_tam;
DROP TABLE pg_batch_groups;
DROP TABLE pg_batch_arrow;
DROP TABLE pg_batch_test;
DROP EXTENSION pg_batch;
DROP EXTENSION pg_batch_tam;
DROP EXTENSION pg_batch_bridge;
