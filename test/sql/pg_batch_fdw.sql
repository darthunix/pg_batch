CREATE EXTENSION pg_batch_bridge;
CREATE EXTENSION pg_batch_fdw;
CREATE EXTENSION pg_batch;

LOAD 'pg_batch_fdw';
LOAD 'pg_batch';

SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TABLE pg_batch_fdw_source AS
SELECT g AS c1,
       CASE WHEN g % 7 = 0 THEN NULL ELSE g + 1 END AS c2,
       g + 3 AS c3,
       g + 4 AS c4,
       g + 5 AS c5,
       g + 6 AS c6,
       g + 7 AS c7,
       g + 8 AS c8
FROM generate_series(1, 130) AS g;

SELECT pg_batch_fdw_remove(current_setting('data_directory') ||
                           '/pg_batch_fdw_' || current_database() || '.arrow');
SELECT pg_batch_fdw_export(
           'pg_batch_fdw_source',
           current_setting('data_directory') ||
           '/pg_batch_fdw_' || current_database() || '.arrow',
           96);

CREATE SERVER pg_batch_fdw_server FOREIGN DATA WRAPPER pg_batch_fdw;
DO $$
BEGIN
  EXECUTE format(
      'CREATE FOREIGN TABLE pg_batch_fdw_foreign '
      '(c1 int, c2 int, c3 int, c4 int, c5 int, c6 int, c7 int, c8 int) '
      'SERVER pg_batch_fdw_server OPTIONS (filename %L)',
      current_setting('data_directory') ||
      '/pg_batch_fdw_' || current_database() || '.arrow');
END
$$;

SET pg_batch.enable = off;
CREATE TEMP TABLE pg_batch_fdw_plain_rows AS
SELECT c1, c8
FROM pg_batch_fdw_foreign
WHERE c2 < 50 AND (c1 + c3) % 5 = 0;
CREATE TEMP TABLE pg_batch_fdw_plain_agg AS
SELECT count(*) AS n, count(c2) AS nn, sum(c8) AS total
FROM pg_batch_fdw_foreign
WHERE c2 < 100;

SET pg_batch.enable = on;
CREATE TEMP TABLE pg_batch_fdw_batch_rows AS
SELECT c1, c8
FROM pg_batch_fdw_foreign
WHERE c2 < 50 AND (c1 + c3) % 5 = 0;
CREATE TEMP TABLE pg_batch_fdw_batch_agg AS
SELECT count(*) AS n, count(c2) AS nn, sum(c8) AS total
FROM pg_batch_fdw_foreign
WHERE c2 < 100;

SELECT NOT EXISTS (
           (TABLE pg_batch_fdw_batch_rows
            EXCEPT ALL TABLE pg_batch_fdw_plain_rows)
           UNION ALL
           (TABLE pg_batch_fdw_plain_rows
            EXCEPT ALL TABLE pg_batch_fdw_batch_rows)
       ) AS rows_match,
       NOT EXISTS (
           (TABLE pg_batch_fdw_batch_agg
            EXCEPT ALL TABLE pg_batch_fdw_plain_agg)
           UNION ALL
           (TABLE pg_batch_fdw_plain_agg
            EXCEPT ALL TABLE pg_batch_fdw_batch_agg)
       ) AS aggregates_match;

-- Exercise every comparison recognized by the source, including a Var on
-- the right side of the operator.
SELECT count(*) AS eq_count FROM pg_batch_fdw_foreign WHERE c1 = 50;
SELECT count(*) AS ne_count FROM pg_batch_fdw_foreign WHERE c1 <> 50;
SELECT count(*) AS lt_count FROM pg_batch_fdw_foreign WHERE c1 < 50;
SELECT count(*) AS le_count FROM pg_batch_fdw_foreign WHERE c1 <= 50;
SELECT count(*) AS gt_count FROM pg_batch_fdw_foreign WHERE c1 > 50;
SELECT count(*) AS ge_count FROM pg_batch_fdw_foreign WHERE 50 <= c1;
SELECT count(*) AS all_rows FROM pg_batch_fdw_foreign;

-- c2 is decoded for both record batches.  Projection columns are decoded
-- only in the first record batch, which contains the surviving rows.
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT c8
FROM pg_batch_fdw_foreign
WHERE c2 < 50 AND (c1 + c3) % 5 = 0;

-- With source pushdown disabled, the batch filter evaluates c2 over all rows.
SET pg_batch_fdw.pushdown = off;
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*), sum(c8)
FROM pg_batch_fdw_foreign
WHERE c2 < 50;
RESET pg_batch_fdw.pushdown;

-- Eager mode is a control showing that all eight columns are decoded.
SET pg_batch_fdw.column_pruning = off;
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*), sum(c8)
FROM pg_batch_fdw_foreign
WHERE c2 < 50;
RESET pg_batch_fdw.column_pruning;

PREPARE pg_batch_fdw_parameter(integer) AS
SELECT count(*), sum(c8)
FROM pg_batch_fdw_foreign
WHERE c2 < $1;
EXECUTE pg_batch_fdw_parameter(10);
EXECUTE pg_batch_fdw_parameter(100);
EXECUTE pg_batch_fdw_parameter(NULL);
DEALLOCATE pg_batch_fdw_parameter;

CREATE TABLE pg_batch_fdw_join(k int, payload int);
INSERT INTO pg_batch_fdw_join
SELECT g, g * 10 FROM generate_series(1, 50) AS g;
ANALYZE pg_batch_fdw_join;
SET enable_nestloop = off;
SET enable_mergejoin = off;

SET pg_batch.enable = off;
CREATE TEMP TABLE pg_batch_fdw_plain_join AS
SELECT count(*) AS n, sum(f.c8) AS foreign_total,
       sum(j.payload) AS local_total
FROM pg_batch_fdw_foreign f
JOIN pg_batch_fdw_join j ON f.c1 = j.k
WHERE f.c2 < 100;

SET pg_batch.enable = on;
CREATE TEMP TABLE pg_batch_fdw_batch_join AS
SELECT count(*) AS n, sum(f.c8) AS foreign_total,
       sum(j.payload) AS local_total
FROM pg_batch_fdw_foreign f
JOIN pg_batch_fdw_join j ON f.c1 = j.k
WHERE f.c2 < 100;

SELECT NOT EXISTS (
           (TABLE pg_batch_fdw_batch_join
            EXCEPT ALL TABLE pg_batch_fdw_plain_join)
           UNION ALL
           (TABLE pg_batch_fdw_plain_join
            EXCEPT ALL TABLE pg_batch_fdw_batch_join)
       ) AS join_match;

RESET enable_mergejoin;
RESET enable_nestloop;
SELECT pg_batch_fdw_remove(current_setting('data_directory') ||
                           '/pg_batch_fdw_' || current_database() || '.arrow');
DROP TABLE pg_batch_fdw_join;
DROP FOREIGN TABLE pg_batch_fdw_foreign;
DROP SERVER pg_batch_fdw_server;
DROP TABLE pg_batch_fdw_source;
DROP EXTENSION pg_batch;
DROP EXTENSION pg_batch_fdw;
DROP EXTENSION pg_batch_bridge;
