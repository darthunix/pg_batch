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
\set repetitions 21
\endif

SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET enable_bitmapscan = off;
SET synchronize_seqscans = off;

SET pg_batch.enable = on;
PREPARE expr_filter_batch AS
SELECT count(*) FROM pg_batch_bench_narrow
WHERE (c1 + 3) * 2 < 2000000;
PREPARE expr_project_batch AS
SELECT sum(c4 + 1) FROM pg_batch_bench_narrow
WHERE c1 < 1000000;
PREPARE expr_combined_batch AS
SELECT sum(c4 + 1) FROM pg_batch_bench_narrow
WHERE (c1 + 3) * 2 < 2000000;
PREPARE expr_wide_batch AS
SELECT sum(c60 + 1) FROM pg_batch_bench_wide
WHERE c2 + 1 < 1000;

SET pg_batch.enable = off;
PREPARE expr_filter_plain AS
SELECT count(*) FROM pg_batch_bench_narrow
WHERE (c1 + 3) * 2 < 2000000;
PREPARE expr_project_plain AS
SELECT sum(c4 + 1) FROM pg_batch_bench_narrow
WHERE c1 < 1000000;
PREPARE expr_combined_plain AS
SELECT sum(c4 + 1) FROM pg_batch_bench_narrow
WHERE (c1 + 3) * 2 < 2000000;
PREPARE expr_wide_plain AS
SELECT sum(c60 + 1) FROM pg_batch_bench_wide
WHERE c2 + 1 < 1000;

CREATE TEMP TABLE pg_batch_expr_timings
(
    test text,
    executor text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_temp.pg_batch_measure_expr(test_name text,
                                              plain_statement text,
                                              batch_statement text,
                                              repetitions integer)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    execution_order text[];
    executor_name text;
    statement_name text;
    started_at timestamptz;
BEGIN
    FOREACH executor_name IN ARRAY ARRAY['postgres', 'batch'] LOOP
        FOR warmup IN 1..5 LOOP
            PERFORM set_config('pg_batch.enable',
                               CASE WHEN executor_name = 'batch'
                                    THEN 'on' ELSE 'off' END, false);
            statement_name := CASE WHEN executor_name = 'batch'
                                   THEN batch_statement
                                   ELSE plain_statement END;
            EXECUTE format('EXECUTE %I', statement_name);
        END LOOP;
    END LOOP;

    FOR sample IN 1..repetitions LOOP
        execution_order := CASE sample % 2
            WHEN 0 THEN ARRAY['postgres', 'batch']
            ELSE ARRAY['batch', 'postgres']
        END;
        FOREACH executor_name IN ARRAY execution_order LOOP
            PERFORM set_config('pg_batch.enable',
                               CASE WHEN executor_name = 'batch'
                                    THEN 'on' ELSE 'off' END, false);
            statement_name := CASE WHEN executor_name = 'batch'
                                   THEN batch_statement
                                   ELSE plain_statement END;
            started_at := clock_timestamp();
            EXECUTE format('EXECUTE %I', statement_name);
            INSERT INTO pg_batch_expr_timings
            VALUES (test_name, executor_name, sample,
                    1000 * extract(epoch FROM clock_timestamp() - started_at));
        END LOOP;
    END LOOP;
END
$function$;

SELECT pg_temp.pg_batch_measure_expr(
    'arithmetic filter and count', 'expr_filter_plain', 'expr_filter_batch',
    :repetitions);
SELECT pg_temp.pg_batch_measure_expr(
    'project expression into sum', 'expr_project_plain', 'expr_project_batch',
    :repetitions);
SELECT pg_temp.pg_batch_measure_expr(
    'arithmetic filter and projected sum',
    'expr_combined_plain', 'expr_combined_batch', :repetitions);
SELECT pg_temp.pg_batch_measure_expr(
    'wide sparse projection expression',
    'expr_wide_plain', 'expr_wide_batch', :repetitions);

SELECT :'variant' AS variant,
       batch.test,
       round(percentile_cont(0.5) WITHIN GROUP
                 (ORDER BY batch.milliseconds)::numeric, 3) AS batch_ms,
       round(percentile_cont(0.5) WITHIN GROUP
                 (ORDER BY postgres.milliseconds)::numeric,
             3) AS postgres_ms,
       round(percentile_cont(0.5) WITHIN GROUP
                 (ORDER BY 100 * batch.milliseconds /
                           postgres.milliseconds)::numeric,
             2) AS batch_percent
FROM pg_batch_expr_timings AS batch
JOIN pg_batch_expr_timings AS postgres
  ON postgres.test = batch.test
 AND postgres.run = batch.run
 AND postgres.executor = 'postgres'
WHERE batch.executor = 'batch'
GROUP BY batch.test
ORDER BY batch.test;

DEALLOCATE ALL;
