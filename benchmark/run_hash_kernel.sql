\set ON_ERROR_STOP on

LOAD 'pg_batch_tam';
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
\set repetitions 11
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
DROP TABLE IF EXISTS pg_batch_hash_probe;
DROP TABLE IF EXISTS pg_batch_hash_build;
DROP TABLE IF EXISTS pg_batch_hash_probe_tam;
DROP TABLE IF EXISTS pg_batch_hash_build_tam;

CREATE TABLE pg_batch_hash_probe(k1 int4, k2 int4, v int4);
CREATE TABLE pg_batch_hash_build(k1 int4, k2 int4, v int4);
INSERT INTO pg_batch_hash_probe
SELECT (g % 200000) + 1, ((g % 200000) + 1) % 17, g
FROM generate_series(1, 1000000) AS g;
INSERT INTO pg_batch_hash_build
SELECT g, g % 17, g * 3
FROM generate_series(1, 200000) AS g;

CREATE TABLE pg_batch_hash_probe_tam
(LIKE pg_batch_hash_probe) USING pg_batch_compressed;
CREATE TABLE pg_batch_hash_build_tam
(LIKE pg_batch_hash_build) USING pg_batch_compressed;
INSERT INTO pg_batch_hash_probe_tam SELECT * FROM pg_batch_hash_probe;
INSERT INTO pg_batch_hash_build_tam SELECT * FROM pg_batch_hash_build;

VACUUM (ANALYZE) pg_batch_hash_probe;
VACUUM (ANALYZE) pg_batch_hash_build;
VACUUM (ANALYZE) pg_batch_hash_probe_tam;
VACUUM (ANALYZE) pg_batch_hash_build_tam;
\endif
SELECT pg_batch_compress('pg_batch_hash_probe_tam');
SELECT pg_batch_compress('pg_batch_hash_build_tam');

CREATE TEMP TABLE pg_batch_hash_timings
(
    test text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_temp.pg_batch_measure_hash(test_name text, query text,
                                              memory_limit text,
                                              repetitions integer)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    started_at timestamptz;
BEGIN
    PERFORM set_config('work_mem', memory_limit, false);
    FOR warmup IN 1..3 LOOP
        EXECUTE query;
    END LOOP;
    FOR sample IN 1..repetitions LOOP
        started_at := clock_timestamp();
        EXECUTE query;
        INSERT INTO pg_batch_hash_timings
        VALUES (test_name, sample,
                1000 * extract(epoch FROM clock_timestamp() - started_at));
    END LOOP;
END
$function$;

SELECT pg_temp.pg_batch_measure_hash(
    'one key, heap, memory',
    $query$SELECT count(*)
           FROM pg_batch_hash_probe p
           JOIN pg_batch_hash_build b ON p.k1 = b.k1$query$,
    '64MB', :repetitions);
SELECT pg_temp.pg_batch_measure_hash(
    'one key, packed, memory',
    $query$SELECT count(*)
           FROM pg_batch_hash_probe_tam p
           JOIN pg_batch_hash_build_tam b ON p.k1 = b.k1$query$,
    '64MB', :repetitions);
SELECT pg_temp.pg_batch_measure_hash(
    'two keys, heap, memory',
    $query$SELECT count(*)
           FROM pg_batch_hash_probe p
           JOIN pg_batch_hash_build b
             ON p.k1 = b.k1 AND p.k2 = b.k2$query$,
    '64MB', :repetitions);
SELECT pg_temp.pg_batch_measure_hash(
    'two keys, packed, memory',
    $query$SELECT count(*)
           FROM pg_batch_hash_probe_tam p
           JOIN pg_batch_hash_build_tam b
             ON p.k1 = b.k1 AND p.k2 = b.k2$query$,
    '64MB', :repetitions);
SELECT pg_temp.pg_batch_measure_hash(
    'one key, heap, sparse probe',
    $query$SELECT count(*)
           FROM pg_batch_hash_probe p
           JOIN pg_batch_hash_build b ON p.k1 = b.k1
           WHERE p.v <= 100000$query$,
    '64MB', :repetitions);
SELECT pg_temp.pg_batch_measure_hash(
    'one key, scalar probe, memory',
    $query$SELECT count(*)
           FROM pg_batch_hash_probe p
           JOIN generate_series(1, 200000) g(k) ON p.k1 = g.k$query$,
    '64MB', :repetitions);
SELECT pg_temp.pg_batch_measure_hash(
    'one key, heap, spill',
    $query$SELECT count(*), sum(p.v), sum(b.v)
           FROM pg_batch_hash_probe p
           JOIN pg_batch_hash_build b ON p.k1 = b.k1$query$,
    '1MB', :repetitions);
SELECT pg_temp.pg_batch_measure_hash(
    'two keys, packed, spill',
    $query$SELECT count(*), sum(p.v), sum(b.v)
           FROM pg_batch_hash_probe_tam p
           JOIN pg_batch_hash_build_tam b
             ON p.k1 = b.k1 AND p.k2 = b.k2$query$,
    '1MB', :repetitions);
SELECT pg_temp.pg_batch_measure_hash(
    'one key, heap, early limit',
    $query$SELECT p.k1
           FROM pg_batch_hash_probe p
           JOIN pg_batch_hash_build b ON p.k1 = b.k1
           WHERE b.k1 <= 1024
           LIMIT 1$query$,
    '64MB', :repetitions);

SELECT :'variant' AS variant,
       test,
       round(percentile_cont(0.5) WITHIN GROUP
             (ORDER BY milliseconds)::numeric, 3) AS median_ms,
       round(percentile_cont(0.1) WITHIN GROUP
             (ORDER BY milliseconds)::numeric, 3) AS p10_ms,
       round(percentile_cont(0.9) WITHIN GROUP
             (ORDER BY milliseconds)::numeric, 3) AS p90_ms
FROM pg_batch_hash_timings
GROUP BY test
ORDER BY test;
