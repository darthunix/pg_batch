\set ON_ERROR_STOP on

LOAD 'pg_batch';
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET synchronize_seqscans = off;

SET pg_batch.enable = on;
PREPARE batch_early_count AS
SELECT count(*) FROM pg_batch_bench_mixed WHERE a > 0;
PREPARE batch_late_count AS
SELECT count(*) FROM pg_batch_bench_mixed WHERE p > 0;
PREPARE batch_early_late_sum AS
SELECT sum(p) FROM pg_batch_bench_mixed WHERE a < 1000;
PREPARE batch_late_early_sum AS
SELECT sum(a) FROM pg_batch_bench_mixed WHERE p < 1058;

SET pg_batch.enable = off;
PREPARE plain_early_count AS
SELECT count(*) FROM pg_batch_bench_mixed WHERE a > 0;
PREPARE plain_late_count AS
SELECT count(*) FROM pg_batch_bench_mixed WHERE p > 0;
PREPARE plain_early_late_sum AS
SELECT sum(p) FROM pg_batch_bench_mixed WHERE a < 1000;
PREPARE plain_late_early_sum AS
SELECT sum(a) FROM pg_batch_bench_mixed WHERE p < 1058;

CREATE TEMP TABLE pg_batch_mixed_times
(
    test text,
    executor text,
    milliseconds double precision
);

DROP FUNCTION IF EXISTS pg_batch_measure_mixed(text, text, text);
CREATE FUNCTION pg_batch_measure_mixed(test_name text,
                                       batch_statement text,
                                       plain_statement text)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    started_at timestamptz;
    first_statement text;
    second_statement text;
    first_executor text;
    second_executor text;
BEGIN
    FOR i IN 1..5 LOOP
        PERFORM set_config('pg_batch.enable', 'on', false);
        EXECUTE format('EXECUTE %I', batch_statement);
        PERFORM set_config('pg_batch.enable', 'off', false);
        EXECUTE format('EXECUTE %I', plain_statement);
    END LOOP;

    FOR i IN 1..31 LOOP
        IF i % 2 = 1 THEN
            first_statement := batch_statement;
            first_executor := 'pg_batch';
            second_statement := plain_statement;
            second_executor := 'postgres';
        ELSE
            first_statement := plain_statement;
            first_executor := 'postgres';
            second_statement := batch_statement;
            second_executor := 'pg_batch';
        END IF;

        started_at := clock_timestamp();
        PERFORM set_config('pg_batch.enable',
                           (first_executor = 'pg_batch')::text, false);
        EXECUTE format('EXECUTE %I', first_statement);
        INSERT INTO pg_batch_mixed_times
        VALUES (test_name, first_executor,
                1000 * extract(epoch FROM clock_timestamp() - started_at));

        started_at := clock_timestamp();
        PERFORM set_config('pg_batch.enable',
                           (second_executor = 'pg_batch')::text, false);
        EXECUTE format('EXECUTE %I', second_statement);
        INSERT INTO pg_batch_mixed_times
        VALUES (test_name, second_executor,
                1000 * extract(epoch FROM clock_timestamp() - started_at));
    END LOOP;
END
$function$;

SELECT pg_batch_measure_mixed('early count',
                              'batch_early_count', 'plain_early_count');
SELECT pg_batch_measure_mixed('late count',
                              'batch_late_count', 'plain_late_count');
SELECT pg_batch_measure_mixed('early filter, late projection',
                              'batch_early_late_sum',
                              'plain_early_late_sum');
SELECT pg_batch_measure_mixed('late filter, early projection',
                              'batch_late_early_sum',
                              'plain_late_early_sum');

SELECT test, executor,
       round(percentile_disc(0.5) WITHIN GROUP (ORDER BY milliseconds)::numeric,
             3) AS median_ms,
       round(avg(milliseconds)::numeric, 3) AS average_ms
FROM pg_batch_mixed_times
GROUP BY test, executor
ORDER BY test, executor;

DROP FUNCTION pg_batch_measure_mixed(text, text, text);
DEALLOCATE ALL;
