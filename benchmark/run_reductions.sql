\set ON_ERROR_STOP on

LOAD 'pg_batch_tam';
\if :{?pg_batch_library}
LOAD :'pg_batch_library';
\else
LOAD 'pg_batch';
\endif
\if :{?pg_batch_label}
\else
\set pg_batch_label batch
\endif

SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET enable_bitmapscan = off;
SET pg_batch_tam.scan_mode = batch;

/* Native snapshots belong to the backend running this benchmark. */
DO $do$
BEGIN
    PERFORM pg_batch_compress('pg_batch_bench_narrow_tam');
    PERFORM pg_batch_compress('pg_batch_bench_plain_tam');
END
$do$;

SET pg_batch.enable = on;
PREPARE batch_heap_count AS
SELECT count(*) FROM pg_batch_bench_narrow;
PREPARE batch_heap_sum AS
SELECT sum(c4) FROM pg_batch_bench_narrow;
PREPARE batch_heap_count_sum AS
SELECT count(c4), sum(c4) FROM pg_batch_bench_narrow;
PREPARE batch_heap_fused AS
SELECT count(c4), sum(c4), min(c4), max(c4)
FROM pg_batch_bench_narrow;
PREPARE batch_heap_two_columns AS
SELECT sum(c4), sum(c8)
FROM pg_batch_bench_narrow;
PREPARE batch_heap_sparse AS
SELECT count(c4), sum(c4)
FROM pg_batch_bench_plain WHERE c1 < 214748364;
PREPARE batch_native_sum AS
SELECT sum(c4) FROM pg_batch_bench_narrow_tam;
PREPARE batch_native_count_sum AS
SELECT count(c4), sum(c4) FROM pg_batch_bench_narrow_tam;
PREPARE batch_native_fused AS
SELECT count(c4), sum(c4), min(c4), max(c4)
FROM pg_batch_bench_narrow_tam;
PREPARE batch_native_sparse AS
SELECT count(c4), sum(c4)
FROM pg_batch_bench_plain_tam WHERE c1 < 214748364;

SET pg_batch.enable = off;
PREPARE postgres_heap_count AS
SELECT count(*) FROM pg_batch_bench_narrow;
PREPARE postgres_heap_sum AS
SELECT sum(c4) FROM pg_batch_bench_narrow;
PREPARE postgres_heap_count_sum AS
SELECT count(c4), sum(c4) FROM pg_batch_bench_narrow;
PREPARE postgres_heap_fused AS
SELECT count(c4), sum(c4), min(c4), max(c4)
FROM pg_batch_bench_narrow;
PREPARE postgres_heap_two_columns AS
SELECT sum(c4), sum(c8)
FROM pg_batch_bench_narrow;
PREPARE postgres_heap_sparse AS
SELECT count(c4), sum(c4)
FROM pg_batch_bench_plain WHERE c1 < 214748364;
PREPARE postgres_native_sum AS
SELECT sum(c4) FROM pg_batch_bench_narrow_tam;
PREPARE postgres_native_count_sum AS
SELECT count(c4), sum(c4) FROM pg_batch_bench_narrow_tam;
PREPARE postgres_native_fused AS
SELECT count(c4), sum(c4), min(c4), max(c4)
FROM pg_batch_bench_narrow_tam;
PREPARE postgres_native_sparse AS
SELECT count(c4), sum(c4)
FROM pg_batch_bench_plain_tam WHERE c1 < 214748364;

CREATE TEMP TABLE pg_batch_reduction_timings
(
    test text,
    executor text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_batch_measure_reduction(test_name text,
                                           postgres_statement text,
                                           batch_statement text,
                                           batch_label text,
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
    FOREACH executor_name IN ARRAY ARRAY['postgres', batch_label] LOOP
        FOR warmup IN 1..5 LOOP
            PERFORM set_config('pg_batch.enable',
                               (executor_name <> 'postgres')::text, false);
            statement_name := CASE WHEN executor_name = 'postgres'
                                   THEN postgres_statement
                                   ELSE batch_statement END;
            EXECUTE format('EXECUTE %I', statement_name);
        END LOOP;
    END LOOP;

    FOR sample IN 1..repetitions LOOP
        execution_order := CASE sample % 2
            WHEN 0 THEN ARRAY['postgres', batch_label]
            ELSE ARRAY[batch_label, 'postgres']
        END;
        FOREACH executor_name IN ARRAY execution_order LOOP
            PERFORM set_config('pg_batch.enable',
                               (executor_name <> 'postgres')::text, false);
            statement_name := CASE WHEN executor_name = 'postgres'
                                   THEN postgres_statement
                                   ELSE batch_statement END;
            started_at := clock_timestamp();
            EXECUTE format('EXECUTE %I', statement_name);
            INSERT INTO pg_batch_reduction_timings
            VALUES (test_name, executor_name, sample,
                    1000 * extract(epoch FROM clock_timestamp() - started_at));
        END LOOP;
    END LOOP;
END
$function$;

SELECT pg_batch_measure_reduction('heap: count star',
                                  'postgres_heap_count', 'batch_heap_count',
                                  :'pg_batch_label');
SELECT pg_batch_measure_reduction('heap: one sum',
                                  'postgres_heap_sum', 'batch_heap_sum',
                                  :'pg_batch_label');
SELECT pg_batch_measure_reduction('heap: count and sum on one column',
                                  'postgres_heap_count_sum',
                                  'batch_heap_count_sum', :'pg_batch_label');
SELECT pg_batch_measure_reduction('heap: four reductions on one column',
                                  'postgres_heap_fused', 'batch_heap_fused',
                                  :'pg_batch_label');
SELECT pg_batch_measure_reduction('heap: sums on two columns',
                                  'postgres_heap_two_columns',
                                  'batch_heap_two_columns', :'pg_batch_label');
SELECT pg_batch_measure_reduction('heap: sparse selected rows',
                                  'postgres_heap_sparse', 'batch_heap_sparse',
                                  :'pg_batch_label');
SELECT pg_batch_measure_reduction('packed int32: one sum',
                                  'postgres_native_sum', 'batch_native_sum',
                                  :'pg_batch_label');
SELECT pg_batch_measure_reduction('packed int32: count and sum',
                                  'postgres_native_count_sum',
                                  'batch_native_count_sum', :'pg_batch_label');
SELECT pg_batch_measure_reduction(
           'packed int32: four reductions on one column',
           'postgres_native_fused', 'batch_native_fused', :'pg_batch_label');
SELECT pg_batch_measure_reduction('packed int32: sparse selected rows',
                                  'postgres_native_sparse',
                                  'batch_native_sparse', :'pg_batch_label');

WITH medians AS
(
    SELECT test, executor,
           percentile_cont(0.5) WITHIN GROUP (ORDER BY milliseconds) AS ms
    FROM pg_batch_reduction_timings
    GROUP BY test, executor
)
SELECT measured.test,
       measured.executor,
       round(measured.ms::numeric, 3) AS median_ms,
       round((100 * measured.ms / baseline.ms - 100)::numeric, 1)
           AS versus_postgres
FROM medians measured
JOIN medians baseline USING (test)
WHERE baseline.executor = 'postgres'
ORDER BY measured.test,
         CASE measured.executor WHEN 'postgres' THEN 1 ELSE 2 END;

DROP FUNCTION pg_batch_measure_reduction(text, text, text, text, integer);
DEALLOCATE ALL;
