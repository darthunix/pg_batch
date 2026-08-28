\set ON_ERROR_STOP on

LOAD 'pg_batch_tam';
LOAD 'pg_batch';
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET enable_bitmapscan = off;
SET pg_batch_tam.scan_mode = batch;

/* Native snapshots are local to the backend running the benchmark. */
DO $do$
BEGIN
    PERFORM pg_batch_compress('pg_batch_bench_narrow_tam');
    PERFORM pg_batch_compress('pg_batch_bench_plain_tam');
END
$do$;

SET pg_batch.enable = on;
PREPARE batch_heap_none AS
SELECT count(*) FROM pg_batch_bench_narrow WHERE c1 < 0;
PREPARE batch_heap_dense AS
SELECT count(*) FROM pg_batch_bench_narrow
WHERE c1 > 100 AND c2 < 1500000;
PREPARE batch_heap_half AS
SELECT count(*) FROM pg_batch_bench_narrow
WHERE c1 <= 1000000 AND c2 > 0;
PREPARE batch_heap_sparse AS
SELECT count(*) FROM pg_batch_bench_plain
WHERE c1 < 107374182 AND c2 > 0;
PREPARE batch_native_none AS
SELECT count(*) FROM pg_batch_bench_narrow_tam WHERE c1 < 0;
PREPARE batch_native_dense AS
SELECT count(*) FROM pg_batch_bench_narrow_tam
WHERE c1 > 100 AND c2 < 1500000;
PREPARE batch_native_sparse AS
SELECT count(*) FROM pg_batch_bench_plain_tam
WHERE c1 < 107374182 AND c2 > 0;

SET pg_batch.enable = off;
PREPARE plain_heap_none AS
SELECT count(*) FROM pg_batch_bench_narrow WHERE c1 < 0;
PREPARE plain_heap_dense AS
SELECT count(*) FROM pg_batch_bench_narrow
WHERE c1 > 100 AND c2 < 1500000;
PREPARE plain_heap_half AS
SELECT count(*) FROM pg_batch_bench_narrow
WHERE c1 <= 1000000 AND c2 > 0;
PREPARE plain_heap_sparse AS
SELECT count(*) FROM pg_batch_bench_plain
WHERE c1 < 107374182 AND c2 > 0;
PREPARE plain_native_none AS
SELECT count(*) FROM pg_batch_bench_narrow_tam WHERE c1 < 0;
PREPARE plain_native_dense AS
SELECT count(*) FROM pg_batch_bench_narrow_tam
WHERE c1 > 100 AND c2 < 1500000;
PREPARE plain_native_sparse AS
SELECT count(*) FROM pg_batch_bench_plain_tam
WHERE c1 < 107374182 AND c2 > 0;

CREATE TEMP TABLE pg_batch_kernel_timings
(
    test text,
    executor text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_batch_measure_kernel(test_name text,
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
BEGIN
    FOREACH executor_name IN ARRAY ARRAY['postgres', 'scalar', 'simd'] LOOP
        FOR warmup IN 1..5 LOOP
            PERFORM set_config('pg_batch.enable',
                               (executor_name <> 'postgres')::text, false);
            PERFORM set_config('pg_batch.enable_simd',
                               (executor_name = 'simd')::text, false);
            statement_name := CASE WHEN executor_name = 'postgres'
                                   THEN plain_statement ELSE batch_statement END;
            EXECUTE format('EXECUTE %I', statement_name);
        END LOOP;
    END LOOP;

    FOR sample IN 1..repetitions LOOP
        execution_order := CASE sample % 3
            WHEN 0 THEN ARRAY['postgres', 'scalar', 'simd']
            WHEN 1 THEN ARRAY['scalar', 'simd', 'postgres']
            ELSE ARRAY['simd', 'postgres', 'scalar']
        END;
        FOREACH executor_name IN ARRAY execution_order LOOP
            PERFORM set_config('pg_batch.enable',
                               (executor_name <> 'postgres')::text, false);
            PERFORM set_config('pg_batch.enable_simd',
                               (executor_name = 'simd')::text, false);
            statement_name := CASE WHEN executor_name = 'postgres'
                                   THEN plain_statement ELSE batch_statement END;
            started_at := clock_timestamp();
            EXECUTE format('EXECUTE %I', statement_name);
            INSERT INTO pg_batch_kernel_timings
            VALUES (test_name, executor_name, sample,
                    1000 * extract(epoch FROM clock_timestamp() - started_at));
        END LOOP;
    END LOOP;
END
$function$;

SELECT pg_batch_measure_kernel('heap: one condition rejects all',
                               'plain_heap_none', 'batch_heap_none');
SELECT pg_batch_measure_kernel('heap: two dense conditions',
                               'plain_heap_dense', 'batch_heap_dense');
SELECT pg_batch_measure_kernel('heap: half survives first condition',
                               'plain_heap_half', 'batch_heap_half');
SELECT pg_batch_measure_kernel('heap: sparse random second condition',
                               'plain_heap_sparse', 'batch_heap_sparse');
SELECT pg_batch_measure_kernel('packed int32: one condition rejects all',
                               'plain_native_none', 'batch_native_none');
SELECT pg_batch_measure_kernel('packed int32: two dense conditions',
                               'plain_native_dense', 'batch_native_dense');
SELECT pg_batch_measure_kernel('packed int32: sparse random second condition',
                               'plain_native_sparse', 'batch_native_sparse');

WITH medians AS
(
    SELECT test, executor,
           percentile_cont(0.5) WITHIN GROUP (ORDER BY milliseconds) AS ms
    FROM pg_batch_kernel_timings
    GROUP BY test, executor
), changes AS
(
    SELECT measured.test, measured.executor, measured.run,
           100 * measured.milliseconds / scalar.milliseconds - 100 AS percent
    FROM pg_batch_kernel_timings AS measured
    JOIN pg_batch_kernel_timings AS scalar
      ON scalar.test = measured.test
     AND scalar.run = measured.run
     AND scalar.executor = 'scalar'
)
SELECT medians.test,
       medians.executor,
       round(medians.ms::numeric, 3) AS median_ms,
       round(percentile_cont(0.5) WITHIN GROUP
                 (ORDER BY changes.percent)::numeric, 2) AS versus_scalar
FROM medians
JOIN changes USING (test, executor)
GROUP BY medians.test, medians.executor, medians.ms
ORDER BY medians.test,
         array_position(ARRAY['postgres', 'scalar', 'simd'], medians.executor);

DROP FUNCTION pg_batch_measure_kernel(text, text, text, integer);
DEALLOCATE ALL;
