\set ON_ERROR_STOP on

LOAD 'pg_batch';
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE INDEX IF NOT EXISTS pg_batch_bench_narrow_c1_brin
ON pg_batch_bench_narrow USING brin(c1) WITH (pages_per_range = 32);
CREATE INDEX IF NOT EXISTS pg_batch_bench_wide_c1_brin
ON pg_batch_bench_wide USING brin(c1) WITH (pages_per_range = 32);
CREATE INDEX IF NOT EXISTS pg_batch_bench_plain_c1_brin
ON pg_batch_bench_plain USING brin(c1) WITH (pages_per_range = 32);
ANALYZE pg_batch_bench_narrow;
ANALYZE pg_batch_bench_wide;
ANALYZE pg_batch_bench_plain;

PREPARE narrow_hit_standard_seq AS
SELECT count(*) FROM pg_batch_bench_narrow
WHERE c1 BETWEEN 1000001 AND 1001000;
PREPARE narrow_hit_batch_seq AS
SELECT count(*) FROM pg_batch_bench_narrow
WHERE c1 BETWEEN 1000001 AND 1001000;
PREPARE narrow_hit_standard_brin AS
SELECT count(*) FROM pg_batch_bench_narrow
WHERE c1 BETWEEN 1000001 AND 1001000;
PREPARE narrow_hit_batch_brin AS
SELECT count(*) FROM pg_batch_bench_narrow
WHERE c1 BETWEEN 1000001 AND 1001000;

PREPARE medium_sum_standard_seq AS
SELECT sum(c4) FROM pg_batch_bench_narrow
WHERE c1 BETWEEN 500001 AND 1000000 AND c2 < 900000;
PREPARE medium_sum_batch_seq AS
SELECT sum(c4) FROM pg_batch_bench_narrow
WHERE c1 BETWEEN 500001 AND 1000000 AND c2 < 900000;
PREPARE medium_sum_standard_brin AS
SELECT sum(c4) FROM pg_batch_bench_narrow
WHERE c1 BETWEEN 500001 AND 1000000 AND c2 < 900000;
PREPARE medium_sum_batch_brin AS
SELECT sum(c4) FROM pg_batch_bench_narrow
WHERE c1 BETWEEN 500001 AND 1000000 AND c2 < 900000;

PREPARE late_projection_standard_seq AS
SELECT sum(c60) FROM pg_batch_bench_wide
WHERE c1 BETWEEN 100001 AND 150000 AND c2 < 125000;
PREPARE late_projection_batch_seq AS
SELECT sum(c60) FROM pg_batch_bench_wide
WHERE c1 BETWEEN 100001 AND 150000 AND c2 < 125000;
PREPARE late_projection_standard_brin AS
SELECT sum(c60) FROM pg_batch_bench_wide
WHERE c1 BETWEEN 100001 AND 150000 AND c2 < 125000;
PREPARE late_projection_batch_brin AS
SELECT sum(c60) FROM pg_batch_bench_wide
WHERE c1 BETWEEN 100001 AND 150000 AND c2 < 125000;

PREPARE range_miss_standard_seq AS
SELECT count(*) FROM pg_batch_bench_narrow WHERE c1 < 0;
PREPARE range_miss_batch_seq AS
SELECT count(*) FROM pg_batch_bench_narrow WHERE c1 < 0;
PREPARE range_miss_standard_brin AS
SELECT count(*) FROM pg_batch_bench_narrow WHERE c1 < 0;
PREPARE range_miss_batch_brin AS
SELECT count(*) FROM pg_batch_bench_narrow WHERE c1 < 0;

PREPARE uncorrelated_standard_seq AS
SELECT count(*) FROM pg_batch_bench_plain
WHERE c1 BETWEEN 1000000 AND 1001000;
PREPARE uncorrelated_batch_seq AS
SELECT count(*) FROM pg_batch_bench_plain
WHERE c1 BETWEEN 1000000 AND 1001000;
PREPARE uncorrelated_standard_brin AS
SELECT count(*) FROM pg_batch_bench_plain
WHERE c1 BETWEEN 1000000 AND 1001000;
PREPARE uncorrelated_batch_brin AS
SELECT count(*) FROM pg_batch_bench_plain
WHERE c1 BETWEEN 1000000 AND 1001000;

CREATE TEMP TABLE pg_batch_brin_timings
(
    test text,
    executor text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_batch_set_brin_executor(executor_name text)
RETURNS void
LANGUAGE plpgsql
AS $function$
BEGIN
    PERFORM set_config('pg_batch.enable',
                       CASE WHEN executor_name LIKE 'batch_%'
                            THEN 'on' ELSE 'off' END, false);
    PERFORM set_config('enable_seqscan',
                       CASE WHEN executor_name LIKE '%_seq'
                            THEN 'on' ELSE 'off' END, false);
    PERFORM set_config('enable_bitmapscan',
                       CASE WHEN executor_name LIKE '%_brin'
                            THEN 'on' ELSE 'off' END, false);
    PERFORM set_config('enable_indexscan', 'off', false);
    PERFORM set_config('enable_indexonlyscan', 'off', false);
END
$function$;

CREATE FUNCTION pg_batch_measure_brin(test_name text,
                                      repetitions integer DEFAULT 15)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    executors constant text[] :=
        ARRAY['standard_seq', 'batch_seq', 'standard_brin', 'batch_brin'];
    execution_order text[];
    executor_name text;
    statement_name text;
    started_at timestamptz;
BEGIN
    FOREACH executor_name IN ARRAY executors LOOP
        PERFORM pg_batch_set_brin_executor(executor_name);
        statement_name := test_name || '_' || executor_name;
        FOR warmup IN 1..3 LOOP
            EXECUTE format('EXECUTE %I', statement_name);
        END LOOP;
    END LOOP;

    FOR sample IN 1..repetitions LOOP
        SELECT array_agg(executors[((i + sample - 2) % 4) + 1] ORDER BY i)
          INTO execution_order
          FROM generate_series(1, 4) AS i;
        FOREACH executor_name IN ARRAY execution_order LOOP
            PERFORM pg_batch_set_brin_executor(executor_name);
            statement_name := test_name || '_' || executor_name;
            started_at := clock_timestamp();
            EXECUTE format('EXECUTE %I', statement_name);
            INSERT INTO pg_batch_brin_timings
            VALUES (test_name, executor_name, sample,
                    1000 * extract(epoch FROM clock_timestamp() - started_at));
        END LOOP;
    END LOOP;
END
$function$;

SELECT pg_batch_measure_brin('narrow_hit');
SELECT pg_batch_measure_brin('medium_sum');
SELECT pg_batch_measure_brin('late_projection');
SELECT pg_batch_measure_brin('range_miss');
SELECT pg_batch_measure_brin('uncorrelated');

SELECT test,
       executor,
       round(percentile_cont(0.5) WITHIN GROUP (ORDER BY milliseconds)::numeric,
             3) AS median_ms
FROM pg_batch_brin_timings
GROUP BY test, executor
ORDER BY test,
         array_position(ARRAY['standard_seq', 'batch_seq',
                              'standard_brin', 'batch_brin'], executor);

SET pg_batch.enable = off;
SET enable_seqscan = off;
SET enable_bitmapscan = on;
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE narrow_hit_standard_brin;

SET pg_batch.enable = on;
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE narrow_hit_batch_brin;

EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE medium_sum_batch_brin;

EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE uncorrelated_batch_brin;

DROP FUNCTION pg_batch_measure_brin(text, integer);
DROP FUNCTION pg_batch_set_brin_executor(text);
DEALLOCATE ALL;
