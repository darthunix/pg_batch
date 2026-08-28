\set ON_ERROR_STOP on

\if :{?batch_library}
LOAD :'batch_library';
\else
LOAD 'pg_batch';
\endif
\if :{?variant}
\else
\set variant current
\endif
\if :{?repetitions}
\else
\set repetitions 31
\endif

SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET enable_bitmapscan = off;
SET synchronize_seqscans = off;
SET pg_batch.enable = on;

/*
 * count(*) avoids column materialization. Its plan passes every 64-row heap
 * batch through PgBatchFilterProject into PgBatchAgg, isolating the common
 * child next/finish protocol as closely as a complete SQL plan permits.
 */
PREPARE pg_batch_input_count AS
SELECT count(*) FROM pg_batch_bench_narrow;

/* Exercise the same protocol after a dense filter changes the selection. */
PREPARE pg_batch_input_filter AS
SELECT count(*) FROM pg_batch_bench_narrow WHERE c1 < 1000000;

/* Keep a late projected column lazy until an early filter has survivors. */
PREPARE pg_batch_input_late_projection AS
SELECT c60 FROM pg_batch_bench_wide WHERE c2 < 1000;

/* Consume one input vector through two fused aggregate states. */
PREPARE pg_batch_input_count_sum AS
SELECT count(c4), sum(c4) FROM pg_batch_bench_narrow;

CREATE TEMP TABLE pg_batch_input_timings
(
    test text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_temp.pg_batch_measure_input(test_name text,
                                               statement_name text,
                                               repetitions integer)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    started_at timestamptz;
    executions_per_sample constant integer := 20;
BEGIN
    FOR warmup IN 1..5 LOOP
        EXECUTE format('EXECUTE %I', statement_name);
    END LOOP;
    FOR sample IN 1..repetitions LOOP
        started_at := clock_timestamp();
        FOR execution IN 1..executions_per_sample LOOP
            EXECUTE format('EXECUTE %I', statement_name);
        END LOOP;
        INSERT INTO pg_batch_input_timings
        VALUES (test_name, sample,
                1000 * extract(epoch FROM clock_timestamp() - started_at) /
                executions_per_sample);
    END LOOP;
END
$function$;

SELECT pg_temp.pg_batch_measure_input(
    'full batches without columns', 'pg_batch_input_count', :repetitions);
SELECT pg_temp.pg_batch_measure_input(
    'dense filter pass-through', 'pg_batch_input_filter', :repetitions);
SELECT pg_temp.pg_batch_measure_input(
    'early filter, late projection',
    'pg_batch_input_late_projection', :repetitions);
SELECT pg_temp.pg_batch_measure_input(
    'count and sum on one column', 'pg_batch_input_count_sum', :repetitions);

SELECT :'variant' AS variant,
       test,
       round(percentile_cont(0.5) WITHIN GROUP
             (ORDER BY milliseconds)::numeric, 3) AS median_ms,
       round(percentile_cont(0.1) WITHIN GROUP
             (ORDER BY milliseconds)::numeric, 3) AS p10_ms,
       round(percentile_cont(0.9) WITHIN GROUP
             (ORDER BY milliseconds)::numeric, 3) AS p90_ms
FROM pg_batch_input_timings
GROUP BY test
ORDER BY test;

DEALLOCATE ALL;
