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
\if :{?setup}
\else
\set setup true
\endif

SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET pg_batch.enable = on;

\if :setup
DROP TABLE IF EXISTS pg_batch_pack_build;
CREATE TABLE pg_batch_pack_build(k int4, v int4);
INSERT INTO pg_batch_pack_build
SELECT g, g * 3 FROM generate_series(1, 200000) AS g;
VACUUM (ANALYZE) pg_batch_pack_build;
\endif

PREPARE pg_batch_pack_one_column AS
SELECT count(*)
FROM generate_series(1, 1000000) AS g(k)
JOIN pg_batch_pack_build b ON g.k = b.k;

PREPARE pg_batch_pack_two_columns AS
SELECT count(*)
FROM ROWS FROM (generate_series(1, 1000003),
                generate_series(2, 1000004)) AS g(k, payload)
JOIN pg_batch_pack_build b
  ON g.k = b.k AND g.payload < b.v;

CREATE TEMP TABLE pg_batch_pack_timings
(
    test text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_temp.pg_batch_measure_pack(test_name text,
                                              statement_name text,
                                              repetitions integer)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    started_at timestamptz;
BEGIN
    FOR warmup IN 1..5 LOOP
        EXECUTE format('EXECUTE %I', statement_name);
    END LOOP;
    FOR sample IN 1..repetitions LOOP
        started_at := clock_timestamp();
        EXECUTE format('EXECUTE %I', statement_name);
        INSERT INTO pg_batch_pack_timings
        VALUES (test_name, sample,
                1000 * extract(epoch FROM clock_timestamp() - started_at));
    END LOOP;
END
$function$;

SELECT pg_temp.pg_batch_measure_pack(
    'one Datum column, exact batches',
    'pg_batch_pack_one_column', :repetitions);
SELECT pg_temp.pg_batch_measure_pack(
    'two Datum columns, partial final batch',
    'pg_batch_pack_two_columns', :repetitions);

SELECT :'variant' AS variant, test,
       round(percentile_disc(0.5) WITHIN GROUP
             (ORDER BY milliseconds), 3) AS median_ms,
       round(percentile_disc(0.1) WITHIN GROUP
             (ORDER BY milliseconds), 3) AS p10_ms,
       round(percentile_disc(0.9) WITHIN GROUP
             (ORDER BY milliseconds), 3) AS p90_ms
FROM pg_batch_pack_timings
GROUP BY test
ORDER BY test;

DEALLOCATE ALL;
