\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS pg_batch_api;
CREATE EXTENSION IF NOT EXISTS pg_batch_fdw;
CREATE EXTENSION IF NOT EXISTS pg_batch;

LOAD 'pg_batch_fdw';
LOAD 'pg_batch';
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;

DROP SERVER IF EXISTS pg_batch_bench_fdw_server CASCADE;
SELECT pg_batch_fdw_remove(current_setting('data_directory') ||
                           '/pg_batch_bench_narrow.arrow');
SELECT pg_batch_fdw_remove(current_setting('data_directory') ||
                           '/pg_batch_bench_wide.arrow');

CREATE TEMP TABLE pg_batch_fdw_export_stats
(
    relation_name text,
    rows bigint,
    milliseconds numeric
);

DO $do$
DECLARE
    started_at timestamptz;
    exported bigint;
BEGIN
    started_at := clock_timestamp();
    exported := pg_batch_fdw_export(
        'pg_batch_bench_narrow',
        current_setting('data_directory') || '/pg_batch_bench_narrow.arrow',
        4096);
    INSERT INTO pg_batch_fdw_export_stats
    VALUES ('pg_batch_bench_narrow', exported,
            1000 * extract(epoch FROM clock_timestamp() - started_at));

    started_at := clock_timestamp();
    exported := pg_batch_fdw_export(
        'pg_batch_bench_wide',
        current_setting('data_directory') || '/pg_batch_bench_wide.arrow',
        4096);
    INSERT INTO pg_batch_fdw_export_stats
    VALUES ('pg_batch_bench_wide', exported,
            1000 * extract(epoch FROM clock_timestamp() - started_at));
END
$do$;

CREATE SERVER pg_batch_bench_fdw_server FOREIGN DATA WRAPPER pg_batch_fdw;
DO $do$
DECLARE
    columns text;
BEGIN
    EXECUTE format(
        'CREATE FOREIGN TABLE pg_batch_bench_narrow_fdw '
        '(c1 int, c2 int, c3 int, c4 int, c5 int, c6 int, c7 int, c8 int) '
        'SERVER pg_batch_bench_fdw_server OPTIONS (filename %L)',
        current_setting('data_directory') || '/pg_batch_bench_narrow.arrow');

    SELECT string_agg(format('c%s int', i), ', ' ORDER BY i)
      INTO columns
      FROM generate_series(1, 60) AS i;
    EXECUTE format(
        'CREATE FOREIGN TABLE pg_batch_bench_wide_fdw (%s) '
        'SERVER pg_batch_bench_fdw_server OPTIONS (filename %L)',
        columns,
        current_setting('data_directory') || '/pg_batch_bench_wide.arrow');
END
$do$;

-- Local heap is a reference point. The other four modes isolate the scalar
-- FDW boundary, batch filtering in the executor, source filtering, and eager
-- Arrow column decoding. Each statement gets its own cached plan because
-- pg_batch.enable and pg_batch_fdw.pushdown affect planning.
SET pg_batch.enable = off;
PREPARE fdw_heap_narrow_dense AS
SELECT count(*), sum(c8) FROM pg_batch_bench_narrow
WHERE c1 > 100 AND c2 < 1500000;
PREPARE fdw_heap_narrow_none AS
SELECT count(*) FROM pg_batch_bench_narrow WHERE c1 < 0;
PREPARE fdw_heap_wide_late_projection AS
SELECT sum(c60) FROM pg_batch_bench_wide WHERE c2 < 1000;
PREPARE fdw_heap_wide_early_projection AS
SELECT sum(c2) FROM pg_batch_bench_wide WHERE c60 < 1058;

SET pg_batch_fdw.pushdown = on;
PREPARE fdw_row_narrow_dense AS
SELECT count(*), sum(c8) FROM pg_batch_bench_narrow_fdw
WHERE c1 > 100 AND c2 < 1500000;
PREPARE fdw_row_narrow_none AS
SELECT count(*) FROM pg_batch_bench_narrow_fdw WHERE c1 < 0;
PREPARE fdw_row_wide_late_projection AS
SELECT sum(c60) FROM pg_batch_bench_wide_fdw WHERE c2 < 1000;
PREPARE fdw_row_wide_early_projection AS
SELECT sum(c2) FROM pg_batch_bench_wide_fdw WHERE c60 < 1058;

SET pg_batch.enable = on;
SET pg_batch_fdw.pushdown = off;
PREPARE fdw_batch_nopush_narrow_dense AS
SELECT count(*), sum(c8) FROM pg_batch_bench_narrow_fdw
WHERE c1 > 100 AND c2 < 1500000;
PREPARE fdw_batch_nopush_narrow_none AS
SELECT count(*) FROM pg_batch_bench_narrow_fdw WHERE c1 < 0;
PREPARE fdw_batch_nopush_wide_late_projection AS
SELECT sum(c60) FROM pg_batch_bench_wide_fdw WHERE c2 < 1000;
PREPARE fdw_batch_nopush_wide_early_projection AS
SELECT sum(c2) FROM pg_batch_bench_wide_fdw WHERE c60 < 1058;

SET pg_batch_fdw.pushdown = on;
PREPARE fdw_batch_push_narrow_dense AS
SELECT count(*), sum(c8) FROM pg_batch_bench_narrow_fdw
WHERE c1 > 100 AND c2 < 1500000;
PREPARE fdw_batch_push_narrow_none AS
SELECT count(*) FROM pg_batch_bench_narrow_fdw WHERE c1 < 0;
PREPARE fdw_batch_push_wide_late_projection AS
SELECT sum(c60) FROM pg_batch_bench_wide_fdw WHERE c2 < 1000;
PREPARE fdw_batch_push_wide_early_projection AS
SELECT sum(c2) FROM pg_batch_bench_wide_fdw WHERE c60 < 1058;

CREATE TEMP TABLE pg_batch_fdw_timings
(
    test text,
    executor text,
    run integer,
    milliseconds numeric
);

CREATE OR REPLACE FUNCTION pg_batch_set_fdw_executor(executor_name text)
RETURNS void
LANGUAGE plpgsql
AS $function$
BEGIN
    PERFORM set_config('pg_batch.enable',
                       CASE WHEN executor_name IN ('heap', 'fdw_row')
                            THEN 'off' ELSE 'on' END, false);
    PERFORM set_config('pg_batch_fdw.pushdown',
                       CASE WHEN executor_name = 'fdw_batch_nopush'
                            THEN 'off' ELSE 'on' END, false);
    PERFORM set_config('pg_batch_fdw.column_pruning',
                       CASE WHEN executor_name = 'fdw_batch_eager'
                            THEN 'off' ELSE 'on' END, false);
END
$function$;

CREATE OR REPLACE FUNCTION pg_batch_measure_fdw(test_name text,
                                                repetitions integer DEFAULT 11)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    executors constant text[] := ARRAY[
        'heap', 'fdw_row', 'fdw_batch_nopush',
        'fdw_batch_push', 'fdw_batch_eager'];
    execution_order text[];
    executor_name text;
    statement_name text;
    started_at timestamptz;
BEGIN
    FOREACH executor_name IN ARRAY executors LOOP
        PERFORM pg_batch_set_fdw_executor(executor_name);
        statement_name := CASE executor_name
            WHEN 'heap' THEN 'fdw_heap_' || test_name
            WHEN 'fdw_batch_eager' THEN 'fdw_batch_push_' || test_name
            ELSE executor_name || '_' || test_name
        END;
        FOR warmup IN 1..3 LOOP
            EXECUTE format('EXECUTE %I', statement_name);
        END LOOP;
    END LOOP;

    FOR sample IN 1..repetitions LOOP
        SELECT array_agg(executors[((i + sample - 2) % 5) + 1] ORDER BY i)
          INTO execution_order
          FROM generate_series(1, 5) AS i;
        FOREACH executor_name IN ARRAY execution_order LOOP
            PERFORM pg_batch_set_fdw_executor(executor_name);
            statement_name := CASE executor_name
                WHEN 'heap' THEN 'fdw_heap_' || test_name
                WHEN 'fdw_batch_eager' THEN 'fdw_batch_push_' || test_name
                ELSE executor_name || '_' || test_name
            END;
            started_at := clock_timestamp();
            EXECUTE format('EXECUTE %I', statement_name);
            INSERT INTO pg_batch_fdw_timings
            VALUES (replace(test_name, '_', ' '), executor_name, sample,
                    1000 * extract(epoch FROM clock_timestamp() - started_at));
        END LOOP;
    END LOOP;
END
$function$;

SELECT pg_batch_measure_fdw('narrow_dense');
SELECT pg_batch_measure_fdw('narrow_none');
SELECT pg_batch_measure_fdw('wide_late_projection');
SELECT pg_batch_measure_fdw('wide_early_projection');

SELECT relation_name, rows, round(milliseconds, 3) AS export_ms
FROM pg_batch_fdw_export_stats ORDER BY relation_name;

SELECT test, executor,
       round(percentile_cont(0.5) WITHIN GROUP (ORDER BY milliseconds)::numeric,
             3) AS median_ms
FROM pg_batch_fdw_timings
GROUP BY test, executor
ORDER BY test,
         array_position(ARRAY[
             'heap', 'fdw_row', 'fdw_batch_nopush',
             'fdw_batch_push', 'fdw_batch_eager'], executor);

SELECT pg_batch_set_fdw_executor('fdw_batch_push');
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE fdw_batch_push_wide_late_projection;

SELECT pg_batch_set_fdw_executor('fdw_batch_nopush');
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE fdw_batch_nopush_wide_late_projection;

SELECT pg_batch_set_fdw_executor('fdw_batch_eager');
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE fdw_batch_push_wide_late_projection;

DROP FUNCTION pg_batch_measure_fdw(text, integer);
DROP FUNCTION pg_batch_set_fdw_executor(text);
DEALLOCATE ALL;
