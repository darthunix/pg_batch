\set ON_ERROR_STOP on

LOAD 'pg_batch';
\if :{?repetitions}
\else
\set repetitions 5
\endif
\if :{?setup}
\else
\set setup false
\endif

SET jit = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET min_parallel_table_scan_size = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;

\if :setup
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
\endif

CREATE TEMP TABLE pg_batch_parallel_join_timings
(
    test text,
    memory text,
    variant text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_temp.pg_batch_measure_parallel_join(
    test_name text, query text, memory_limit text, names text[],
    batch_enabled boolean[], workers integer[], repetitions integer)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    execution_order integer[];
    executor_index integer;
    started_at timestamptz;
BEGIN
    FOR executor_index IN 1..array_length(names, 1) LOOP
        PERFORM set_config('work_mem', memory_limit, false);
        PERFORM set_config('pg_batch.enable',
                           CASE WHEN batch_enabled[executor_index]
                               THEN 'on' ELSE 'off' END, false);
        PERFORM set_config('max_parallel_workers_per_gather',
                           workers[executor_index]::text, false);
        EXECUTE query;
    END LOOP;

    FOR sample IN 1..repetitions LOOP
        SELECT array_agg(i ORDER BY
                         CASE WHEN sample % 2 = 0 THEN -i ELSE i END)
        INTO execution_order
        FROM generate_series(1, array_length(names, 1)) AS i;
        FOREACH executor_index IN ARRAY execution_order LOOP
            PERFORM set_config('work_mem', memory_limit, false);
            PERFORM set_config('pg_batch.enable',
                               CASE WHEN batch_enabled[executor_index]
                                   THEN 'on' ELSE 'off' END, false);
            PERFORM set_config('max_parallel_workers_per_gather',
                               workers[executor_index]::text, false);
            started_at := clock_timestamp();
            EXECUTE query;
            INSERT INTO pg_batch_parallel_join_timings
            VALUES (test_name, memory_limit, names[executor_index], sample,
                    1000 * extract(epoch FROM
                                   clock_timestamp() - started_at));
        END LOOP;
    END LOOP;
END
$function$;

SELECT pg_temp.pg_batch_measure_parallel_join(
    'one join',
    'SELECT count(*), sum(p.v), sum(b.v) '
    'FROM pg_batch_join_probe_large p '
    'JOIN pg_batch_join_build_large b ON p.k = b.k',
    memory_limit,
    ARRAY['postgres serial', 'postgres p4', 'batch serial',
          'batch p1', 'batch p2', 'batch p4'],
    ARRAY[false, false, true, true, true, true],
    ARRAY[0, 4, 0, 1, 2, 4], :repetitions)
FROM unnest(ARRAY['64MB', '8MB']) AS memory_limit;

SELECT pg_temp.pg_batch_measure_parallel_join(
    'two-join chain',
    'SELECT count(*), sum(p.v), sum(b1.v), sum(b2.v) '
    'FROM pg_batch_join_probe_large p '
    'JOIN pg_batch_join_build_large b1 ON p.k = b1.k '
    'JOIN pg_batch_join_build_large b2 ON p.k = b2.k',
    memory_limit,
    ARRAY['postgres p4', 'batch serial', 'batch p2', 'batch p4'],
    ARRAY[false, true, true, true], ARRAY[4, 0, 2, 4], :repetitions)
FROM unnest(ARRAY['64MB', '8MB']) AS memory_limit;

WITH medians AS
(
    SELECT test, memory, variant,
           percentile_cont(0.5) WITHIN GROUP
               (ORDER BY milliseconds) AS milliseconds
    FROM pg_batch_parallel_join_timings
    GROUP BY test, memory, variant
), serial AS
(
    SELECT test, memory, milliseconds
    FROM medians
    WHERE variant = 'batch serial'
)
SELECT measured.test, measured.memory, measured.variant,
       round(measured.milliseconds::numeric, 3) AS median_ms,
       round((measured.milliseconds / serial.milliseconds)::numeric, 3)
           AS vs_batch_serial
FROM medians measured
JOIN serial USING (test, memory)
ORDER BY measured.test, measured.memory, measured.variant;

SET pg_batch.enable = on;
SET max_parallel_workers_per_gather = 4;
SET work_mem = '64MB';
EXPLAIN (COSTS OFF)
SELECT count(*), sum(p.v), sum(b1.v), sum(b2.v)
FROM pg_batch_join_probe_large p
JOIN pg_batch_join_build_large b1 ON p.k = b1.k
JOIN pg_batch_join_build_large b2 ON p.k = b2.k;
