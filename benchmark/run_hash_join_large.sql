\set ON_ERROR_STOP on

LOAD 'pg_batch_tam';
LOAD 'pg_batch';
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET work_mem = '8MB';

DROP TABLE IF EXISTS pg_batch_join_probe_large;
DROP TABLE IF EXISTS pg_batch_join_build_large;
CREATE TABLE pg_batch_join_probe_large(k int4, v int4);
CREATE TABLE pg_batch_join_build_large(k int4, v int4);
INSERT INTO pg_batch_join_probe_large
SELECT (g % 2000000) + 1, g FROM generate_series(1, 10000000) AS g;
INSERT INTO pg_batch_join_build_large
SELECT g, g * 3 FROM generate_series(1, 2000000) AS g;
VACUUM (ANALYZE) pg_batch_join_probe_large;
VACUUM (ANALYZE) pg_batch_join_build_large;

CREATE TEMP TABLE pg_batch_large_join_timings
(executor text, run integer, milliseconds numeric);

DO $body$
DECLARE
    names text[] := ARRAY['postgres', 'heap batch'];
    enabled boolean[] := ARRAY[false, true];
    execution_order integer[];
    executor_index integer;
    started_at timestamptz;
    query text :=
        'SELECT count(*), sum(p.v), sum(b.v) '
        'FROM pg_batch_join_probe_large p '
        'JOIN pg_batch_join_build_large b ON p.k = b.k';
BEGIN
    FOR executor_index IN 1..2 LOOP
        PERFORM set_config('pg_batch.enable',
                           CASE WHEN enabled[executor_index]
                               THEN 'on' ELSE 'off' END, false);
        FOR warmup IN 1..2 LOOP
            EXECUTE query;
        END LOOP;
    END LOOP;
    FOR sample IN 1..7 LOOP
        execution_order := CASE sample % 2
            WHEN 0 THEN ARRAY[1, 2]
            ELSE ARRAY[2, 1]
        END;
        FOREACH executor_index IN ARRAY execution_order LOOP
            PERFORM set_config('pg_batch.enable',
                               CASE WHEN enabled[executor_index]
                                   THEN 'on' ELSE 'off' END, false);
            started_at := clock_timestamp();
            EXECUTE query;
            INSERT INTO pg_batch_large_join_timings
            VALUES (names[executor_index], sample,
                    1000 * extract(epoch FROM clock_timestamp() - started_at));
        END LOOP;
    END LOOP;
END
$body$;

WITH medians AS
(
    SELECT executor,
           percentile_cont(0.5) WITHIN GROUP (ORDER BY milliseconds) AS ms
    FROM pg_batch_large_join_timings
    GROUP BY executor
)
SELECT measured.executor,
       round(measured.ms::numeric, 3) AS median_ms,
       round((100 * measured.ms / baseline.ms)::numeric, 1) AS postgres_percent
FROM medians measured
CROSS JOIN (SELECT ms FROM medians WHERE executor = 'postgres') baseline
ORDER BY CASE measured.executor WHEN 'postgres' THEN 1 ELSE 2 END;

SET pg_batch.enable = on;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY ON)
SELECT count(*), sum(p.v), sum(b.v)
FROM pg_batch_join_probe_large p
JOIN pg_batch_join_build_large b ON p.k = b.k;
