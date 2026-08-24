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

DROP TABLE pg_batch_test;
DROP EXTENSION pg_batch;
