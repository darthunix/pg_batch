\set ON_ERROR_STOP on

LOAD 'pg_batch';
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET enable_bitmapscan = off;

SET pg_batch.enable = on;
PREPARE batch_narrow_dense AS
SELECT count(*) FROM pg_batch_bench_narrow
WHERE c1 > 100 AND c2 < 1500000;
PREPARE batch_narrow_none AS
SELECT count(*) FROM pg_batch_bench_narrow WHERE c1 < 0;
PREPARE batch_wide_late_projection AS
SELECT c60 FROM pg_batch_bench_wide WHERE c2 < 1000;
PREPARE batch_wide_early_projection AS
SELECT c2 FROM pg_batch_bench_wide WHERE c60 < 1058;
PREPARE batch_wide_count AS
SELECT count(*) FROM pg_batch_bench_wide WHERE c2 < 1000;

SET pg_batch.enable = off;
PREPARE plain_narrow_dense AS
SELECT count(*) FROM pg_batch_bench_narrow
WHERE c1 > 100 AND c2 < 1500000;
PREPARE plain_narrow_none AS
SELECT count(*) FROM pg_batch_bench_narrow WHERE c1 < 0;
PREPARE plain_wide_late_projection AS
SELECT c60 FROM pg_batch_bench_wide WHERE c2 < 1000;
PREPARE plain_wide_early_projection AS
SELECT c2 FROM pg_batch_bench_wide WHERE c60 < 1058;
PREPARE plain_wide_count AS
SELECT count(*) FROM pg_batch_bench_wide WHERE c2 < 1000;

CREATE TEMP TABLE pg_batch_heap_timings
(
    test text,
    executor text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_batch_measure_heap(test_name text,
                                      plain_statement text,
                                      batch_statement text,
                                      repetitions integer DEFAULT 31)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    execution_order text[];
    executor_name text;
    statement_name text;
    started_at timestamptz;
    executions_per_sample constant integer := 10;
BEGIN
    FOREACH executor_name IN ARRAY ARRAY['postgres', 'heap'] LOOP
        FOR warmup IN 1..5 LOOP
            PERFORM set_config('pg_batch.enable',
                               CASE WHEN executor_name = 'heap'
                                    THEN 'on' ELSE 'off' END, false);
            statement_name := CASE WHEN executor_name = 'heap'
                                   THEN batch_statement ELSE plain_statement END;
            EXECUTE format('EXECUTE %I', statement_name);
        END LOOP;
    END LOOP;

    FOR sample IN 1..repetitions LOOP
        execution_order := CASE sample % 2
            WHEN 0 THEN ARRAY['postgres', 'heap']
            ELSE ARRAY['heap', 'postgres']
        END;
        FOREACH executor_name IN ARRAY execution_order LOOP
            PERFORM set_config('pg_batch.enable',
                               CASE WHEN executor_name = 'heap'
                                    THEN 'on' ELSE 'off' END, false);
            statement_name := CASE WHEN executor_name = 'heap'
                                   THEN batch_statement ELSE plain_statement END;
            started_at := clock_timestamp();
            FOR execution IN 1..executions_per_sample LOOP
                EXECUTE format('EXECUTE %I', statement_name);
            END LOOP;
            INSERT INTO pg_batch_heap_timings
            VALUES (test_name, executor_name, sample,
                    1000 * extract(epoch FROM clock_timestamp() - started_at) /
                    executions_per_sample);
        END LOOP;
    END LOOP;
END
$function$;

SELECT pg_batch_measure_heap('narrow dense quals',
                             'plain_narrow_dense', 'batch_narrow_dense');
SELECT pg_batch_measure_heap('narrow pass none',
                             'plain_narrow_none', 'batch_narrow_none');
SELECT pg_batch_measure_heap('wide early filter late projection',
                             'plain_wide_late_projection',
                             'batch_wide_late_projection');
SELECT pg_batch_measure_heap('wide late filter early projection',
                             'plain_wide_early_projection',
                             'batch_wide_early_projection');
SELECT pg_batch_measure_heap('wide count without projection',
                             'plain_wide_count', 'batch_wide_count');

SELECT heap.test,
       round(percentile_cont(0.5) WITHIN GROUP
                 (ORDER BY heap.milliseconds)::numeric, 3) AS heap_ms,
       round(percentile_cont(0.5) WITHIN GROUP
                 (ORDER BY postgres.milliseconds)::numeric, 3) AS postgres_ms,
       round(percentile_cont(0.5) WITHIN GROUP
                 (ORDER BY 100 * heap.milliseconds / postgres.milliseconds)::numeric,
             2) AS heap_percent
FROM pg_batch_heap_timings AS heap
JOIN pg_batch_heap_timings AS postgres
  ON postgres.test = heap.test
 AND postgres.run = heap.run
 AND postgres.executor = 'postgres'
WHERE heap.executor = 'heap'
GROUP BY heap.test
ORDER BY heap.test;

DROP FUNCTION pg_batch_measure_heap(text, text, text, integer);
DEALLOCATE ALL;
