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
PREPARE batch_narrow_residual AS
SELECT c8 FROM pg_batch_bench_narrow WHERE c1 % 1000 = 0;
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
PREPARE plain_narrow_residual AS
SELECT c8 FROM pg_batch_bench_narrow WHERE c1 % 1000 = 0;
PREPARE plain_wide_late_projection AS
SELECT c60 FROM pg_batch_bench_wide WHERE c2 < 1000;
PREPARE plain_wide_early_projection AS
SELECT c2 FROM pg_batch_bench_wide WHERE c60 < 1058;
PREPARE plain_wide_count AS
SELECT count(*) FROM pg_batch_bench_wide WHERE c2 < 1000;

CREATE OR REPLACE FUNCTION pg_batch_measure(
    test_name text,
    batch_enabled boolean,
    statement_name text,
    repetitions integer DEFAULT 5)
RETURNS TABLE(test text, executor text, milliseconds numeric)
LANGUAGE plpgsql
AS $function$
DECLARE
    started_at timestamptz;
BEGIN
    PERFORM set_config('pg_batch.enable', batch_enabled::text, false);

    /* Warm the relation and initialize expression and function caches. */
    EXECUTE format('EXECUTE %I', statement_name);

    started_at := clock_timestamp();
    FOR i IN 1..repetitions LOOP
        EXECUTE format('EXECUTE %I', statement_name);
    END LOOP;

    test := test_name;
    executor := CASE WHEN batch_enabled THEN 'pg_batch' ELSE 'postgres' END;
    milliseconds := round(
        1000 * extract(epoch FROM clock_timestamp() - started_at) /
        repetitions,
        3);
    RETURN NEXT;
END
$function$;

SELECT * FROM pg_batch_measure(
    'narrow dense quals', true,
    'batch_narrow_dense');
SELECT * FROM pg_batch_measure(
    'narrow dense quals', false,
    'plain_narrow_dense');

SELECT * FROM pg_batch_measure(
    'narrow pass none', true,
    'batch_narrow_none');
SELECT * FROM pg_batch_measure(
    'narrow pass none', false,
    'plain_narrow_none');

SELECT * FROM pg_batch_measure(
    'narrow residual fallback', true,
    'batch_narrow_residual');
SELECT * FROM pg_batch_measure(
    'narrow residual fallback', false,
    'plain_narrow_residual');

SELECT * FROM pg_batch_measure(
    'wide early filter late projection', true,
    'batch_wide_late_projection');
SELECT * FROM pg_batch_measure(
    'wide early filter late projection', false,
    'plain_wide_late_projection');

SELECT * FROM pg_batch_measure(
    'wide late filter early projection', true,
    'batch_wide_early_projection');
SELECT * FROM pg_batch_measure(
    'wide late filter early projection', false,
    'plain_wide_early_projection');

SELECT * FROM pg_batch_measure(
    'wide count without projection', true,
    'batch_wide_count');
SELECT * FROM pg_batch_measure(
    'wide count without projection', false,
    'plain_wide_count');

DROP FUNCTION pg_batch_measure(text, boolean, text, integer);
DEALLOCATE ALL;
