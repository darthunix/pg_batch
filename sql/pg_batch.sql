CREATE EXTENSION pg_batch;
LOAD 'pg_batch';

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

CREATE TABLE pg_batch_arrow AS
SELECT g AS c1,
       CASE WHEN g % 7 = 0 THEN NULL ELSE g + 1 END AS c2,
       CASE WHEN g % 2 = 0
            THEN 2147483647 - g
            ELSE -2147483647 + g
       END AS c3,
       g * 1000 AS c4
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
SET pg_batch.use_compressed = on;

EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*), count(c2), sum(c1), sum(c4)
FROM pg_batch_arrow
WHERE c2 < 100;

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

CREATE TABLE pg_batch_groups AS
SELECT g AS c1, (g % 4) * 2 AS c2, g + 1000 AS c3
FROM generate_series(1, 384) AS g;
ANALYZE pg_batch_groups;
SELECT pg_batch_compress('pg_batch_groups', 128) > 0 AS grouped;

SET pg_batch.compressed_scan_mode = prune;
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) FROM pg_batch_groups WHERE c1 >= 130 AND c1 <= 140;

EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) FROM pg_batch_groups WHERE c2 = 3;

SET pg_batch.compressed_scan_mode = filter;
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
SET pg_batch.compressed_via_tableam = on;
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*), sum(c3) FROM pg_batch_tam WHERE c2 = 2 AND c1 > 100;

CREATE TEMP TABLE smart_tam AS
SELECT c1, c3 FROM pg_batch_tam WHERE c2 = 2 AND c1 > 100;

SELECT NOT EXISTS (
           (TABLE smart_tam EXCEPT ALL TABLE smart_plain)
           UNION ALL
           (TABLE smart_plain EXCEPT ALL TABLE smart_tam)
       ) AS tam_rows_match;

DROP TABLE pg_batch_tam;
DROP TABLE pg_batch_groups;
DROP TABLE pg_batch_arrow;
DROP TABLE pg_batch_test;
DROP EXTENSION pg_batch;
