\set ON_ERROR_STOP on

LOAD 'pg_batch_tam';
LOAD 'pg_batch';
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;

DROP TABLE IF EXISTS pg_batch_join_probe;
DROP TABLE IF EXISTS pg_batch_join_build;
DROP TABLE IF EXISTS pg_batch_join_probe_tam;
DROP TABLE IF EXISTS pg_batch_join_build_tam;
DROP TABLE IF EXISTS pg_batch_join_probe_50;
DROP TABLE IF EXISTS pg_batch_join_probe_10;
DROP TABLE IF EXISTS pg_batch_join_probe_miss;
DROP TABLE IF EXISTS pg_batch_join_probe_wide;
DROP TABLE IF EXISTS pg_batch_join_build_wide;

CREATE TABLE pg_batch_join_probe(k int4, v int4);
CREATE TABLE pg_batch_join_build(k int4, v int4);
INSERT INTO pg_batch_join_probe
SELECT (g % 200000) + 1, g FROM generate_series(1, 1000000) AS g;
INSERT INTO pg_batch_join_build
SELECT g, g * 3 FROM generate_series(1, 200000) AS g;

CREATE TABLE pg_batch_join_probe_50(k int4, v int4);
CREATE TABLE pg_batch_join_probe_10(k int4, v int4);
CREATE TABLE pg_batch_join_probe_miss(k int4, v int4);
INSERT INTO pg_batch_join_probe_50
SELECT CASE WHEN g % 2 = 0 THEN (g % 200000) + 1 ELSE 200000 + g END, g
FROM generate_series(1, 1000000) AS g;
INSERT INTO pg_batch_join_probe_10
SELECT CASE WHEN g % 10 = 0 THEN (g % 200000) + 1 ELSE 200000 + g END, g
FROM generate_series(1, 1000000) AS g;
INSERT INTO pg_batch_join_probe_miss
SELECT 200000 + g, g FROM generate_series(1, 1000000) AS g;

CREATE TABLE pg_batch_join_probe_wide
(k int4, v1 int4, v2 int4, v3 int4, v4 int4,
 v5 int4, v6 int4, v7 int4, v8 int4);
CREATE TABLE pg_batch_join_build_wide (LIKE pg_batch_join_probe_wide);
INSERT INTO pg_batch_join_probe_wide
SELECT (g % 200000) + 1,
       g, g + 1, g + 2, g + 3, g + 4, g + 5, g + 6, g + 7
FROM generate_series(1, 1000000) AS g;
INSERT INTO pg_batch_join_build_wide
SELECT g,
       g * 2, g * 2 + 1, g * 2 + 2, g * 2 + 3,
       g * 2 + 4, g * 2 + 5, g * 2 + 6, g * 2 + 7
FROM generate_series(1, 200000) AS g;

CREATE TABLE pg_batch_join_probe_tam
(LIKE pg_batch_join_probe)
USING pg_batch_compressed;
CREATE TABLE pg_batch_join_build_tam
(LIKE pg_batch_join_build)
USING pg_batch_compressed;
INSERT INTO pg_batch_join_probe_tam SELECT * FROM pg_batch_join_probe;
INSERT INTO pg_batch_join_build_tam SELECT * FROM pg_batch_join_build;

VACUUM (ANALYZE) pg_batch_join_probe;
VACUUM (ANALYZE) pg_batch_join_build;
VACUUM (ANALYZE) pg_batch_join_probe_tam;
VACUUM (ANALYZE) pg_batch_join_build_tam;
VACUUM (ANALYZE) pg_batch_join_probe_50;
VACUUM (ANALYZE) pg_batch_join_probe_10;
VACUUM (ANALYZE) pg_batch_join_probe_miss;
VACUUM (ANALYZE) pg_batch_join_probe_wide;
VACUUM (ANALYZE) pg_batch_join_build_wide;
SELECT pg_batch_compress('pg_batch_join_probe_tam');
SELECT pg_batch_compress('pg_batch_join_build_tam');

CREATE TEMP TABLE pg_batch_join_timings
(
    test text,
    executor text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_batch_measure_join(test_name text,
                                      executor_names text[],
                                      batch_enabled boolean[],
                                      queries text[],
                                      memory_limit text,
                                      repetitions integer DEFAULT 11)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    execution_order integer[];
    executor_index integer;
    started_at timestamptz;
BEGIN
    FOR executor_index IN 1..array_length(executor_names, 1) LOOP
        FOR warmup IN 1..3 LOOP
            PERFORM set_config('work_mem', memory_limit, false);
            PERFORM set_config('pg_batch.enable',
                               CASE WHEN batch_enabled[executor_index]
                                   THEN 'on' ELSE 'off' END, false);
            EXECUTE queries[executor_index];
        END LOOP;
    END LOOP;

    FOR sample IN 1..repetitions LOOP
        IF array_length(executor_names, 1) = 3 THEN
            execution_order := CASE sample % 3
                WHEN 0 THEN ARRAY[1, 2, 3]
                WHEN 1 THEN ARRAY[2, 3, 1]
                ELSE ARRAY[3, 1, 2]
            END;
        ELSE
            execution_order := CASE sample % 2
                WHEN 0 THEN ARRAY[1, 2]
                ELSE ARRAY[2, 1]
            END;
        END IF;
        FOREACH executor_index IN ARRAY execution_order LOOP
            PERFORM set_config('work_mem', memory_limit, false);
            PERFORM set_config('pg_batch.enable',
                               CASE WHEN batch_enabled[executor_index]
                                   THEN 'on' ELSE 'off' END, false);
            started_at := clock_timestamp();
            EXECUTE queries[executor_index];
            INSERT INTO pg_batch_join_timings
            VALUES (test_name, executor_names[executor_index], sample,
                    1000 * extract(epoch FROM clock_timestamp() - started_at));
        END LOOP;
    END LOOP;
END
$function$;

SELECT pg_batch_measure_join(
    'in memory', ARRAY['postgres', 'heap batch', 'arrow batch'],
    ARRAY[false, true, true],
    ARRAY[
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe p
               JOIN pg_batch_join_build b ON p.k = b.k
               WHERE p.v > 100$query$,
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe p
               JOIN pg_batch_join_build b ON p.k = b.k
               WHERE p.v > 100$query$,
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe_tam p
               JOIN pg_batch_join_build_tam b ON p.k = b.k
               WHERE p.v > 100$query$
    ], '64MB');
SELECT pg_batch_measure_join(
    'count only', ARRAY['postgres', 'heap batch', 'arrow batch'],
    ARRAY[false, true, true],
    ARRAY[
        $query$SELECT count(*)
               FROM pg_batch_join_probe p
               JOIN pg_batch_join_build b ON p.k = b.k$query$,
        $query$SELECT count(*)
               FROM pg_batch_join_probe p
               JOIN pg_batch_join_build b ON p.k = b.k$query$,
        $query$SELECT count(*)
               FROM pg_batch_join_probe_tam p
               JOIN pg_batch_join_build_tam b ON p.k = b.k$query$
    ], '64MB');
SELECT pg_batch_measure_join(
    'join residual', ARRAY['postgres', 'heap batch', 'arrow batch'],
    ARRAY[false, true, true],
    ARRAY[
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe p
               JOIN pg_batch_join_build b
                 ON p.k = b.k AND p.v < b.v$query$,
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe p
               JOIN pg_batch_join_build b
                 ON p.k = b.k AND p.v < b.v$query$,
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe_tam p
               JOIN pg_batch_join_build_tam b
                 ON p.k = b.k AND p.v < b.v$query$
    ], '64MB');
SELECT pg_batch_measure_join(
    'spill payload filter', ARRAY['postgres', 'heap batch', 'arrow batch'],
    ARRAY[false, true, true],
    ARRAY[
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe p
               JOIN pg_batch_join_build b ON p.k = b.k
               WHERE p.v > 100$query$,
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe p
               JOIN pg_batch_join_build b ON p.k = b.k
               WHERE p.v > 100$query$,
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe_tam p
               JOIN pg_batch_join_build_tam b ON p.k = b.k
               WHERE p.v > 100$query$
    ], '1MB', 31);
SELECT pg_batch_measure_join(
    'spill 100%, 2MB', ARRAY['postgres', 'heap batch'],
    ARRAY[false, true],
    ARRAY[
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe p
               JOIN pg_batch_join_build b ON p.k = b.k
               WHERE p.v > 100$query$,
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe p
               JOIN pg_batch_join_build b ON p.k = b.k
               WHERE p.v > 100$query$
    ], '2MB', 31);
SELECT pg_batch_measure_join(
    'spill 100%, 4MB', ARRAY['postgres', 'heap batch'],
    ARRAY[false, true],
    ARRAY[
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe p
               JOIN pg_batch_join_build b ON p.k = b.k
               WHERE p.v > 100$query$,
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe p
               JOIN pg_batch_join_build b ON p.k = b.k
               WHERE p.v > 100$query$
    ], '4MB', 31);
SELECT pg_batch_measure_join(
    'spill 100% match', ARRAY['postgres', 'heap batch'],
    ARRAY[false, true],
    ARRAY[
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe p
               JOIN pg_batch_join_build b ON p.k = b.k$query$,
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe p
               JOIN pg_batch_join_build b ON p.k = b.k$query$
    ], '1MB', 31);
SELECT pg_batch_measure_join(
    'spill 50% match', ARRAY['postgres', 'heap batch'],
    ARRAY[false, true],
    ARRAY[
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe_50 p
               JOIN pg_batch_join_build b ON p.k = b.k$query$,
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe_50 p
               JOIN pg_batch_join_build b ON p.k = b.k$query$
    ], '1MB', 31);
SELECT pg_batch_measure_join(
    'spill 10% match', ARRAY['postgres', 'heap batch'],
    ARRAY[false, true],
    ARRAY[
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe_10 p
               JOIN pg_batch_join_build b ON p.k = b.k$query$,
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe_10 p
               JOIN pg_batch_join_build b ON p.k = b.k$query$
    ], '1MB', 31);
SELECT pg_batch_measure_join(
    'spill 0% match', ARRAY['postgres', 'heap batch'],
    ARRAY[false, true],
    ARRAY[
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe_miss p
               JOIN pg_batch_join_build b ON p.k = b.k$query$,
        $query$SELECT count(*), sum(p.v), sum(b.v)
               FROM pg_batch_join_probe_miss p
               JOIN pg_batch_join_build b ON p.k = b.k$query$
    ], '1MB', 31);
SELECT pg_batch_measure_join(
    'wide spill', ARRAY['postgres', 'heap batch'],
    ARRAY[false, true],
    ARRAY[
        $query$SELECT count(*),
                      sum(p.v1), sum(p.v2), sum(p.v3), sum(p.v4),
                      sum(p.v5), sum(p.v6), sum(p.v7), sum(p.v8),
                      sum(b.v1), sum(b.v2), sum(b.v3), sum(b.v4),
                      sum(b.v5), sum(b.v6), sum(b.v7), sum(b.v8)
               FROM pg_batch_join_probe_wide p
               JOIN pg_batch_join_build_wide b ON p.k = b.k$query$,
        $query$SELECT count(*),
                      sum(p.v1), sum(p.v2), sum(p.v3), sum(p.v4),
                      sum(p.v5), sum(p.v6), sum(p.v7), sum(p.v8),
                      sum(b.v1), sum(b.v2), sum(b.v3), sum(b.v4),
                      sum(b.v5), sum(b.v6), sum(b.v7), sum(b.v8)
               FROM pg_batch_join_probe_wide p
               JOIN pg_batch_join_build_wide b ON p.k = b.k$query$
    ], '1MB', 11);

WITH medians AS
(
    SELECT test, executor,
           percentile_cont(0.5) WITHIN GROUP (ORDER BY milliseconds) AS ms
    FROM pg_batch_join_timings
    GROUP BY test, executor
)
SELECT measured.test,
       measured.executor,
       round(measured.ms::numeric, 3) AS median_ms,
       round((100 * measured.ms / baseline.ms)::numeric, 1) AS postgres_percent
FROM medians measured
JOIN medians baseline USING (test)
WHERE baseline.executor = 'postgres'
ORDER BY measured.test,
         CASE measured.executor
             WHEN 'postgres' THEN 1
             WHEN 'heap batch' THEN 2
             ELSE 3
         END;

DROP FUNCTION pg_batch_measure_join(text, text[], boolean[], text[], text,
                                    integer);
