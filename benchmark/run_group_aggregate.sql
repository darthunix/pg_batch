\set ON_ERROR_STOP on

LOAD 'pg_batch';
SET max_parallel_workers_per_gather = 0;
SET jit = off;

DROP TABLE IF EXISTS pg_batch_group_aggregate;
CREATE TABLE pg_batch_group_aggregate
(
    small_key int4,
    large_key int4,
    value int4
);
INSERT INTO pg_batch_group_aggregate
SELECT g % 1000,
       g % 200000,
       CASE WHEN g % 97 = 0 THEN NULL ELSE g END
FROM generate_series(1, 1000000) AS g;
VACUUM (ANALYZE) pg_batch_group_aggregate;

CREATE TEMP TABLE pg_batch_group_aggregate_timings
(
    test text,
    executor text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_temp.pg_batch_measure_group_aggregate(
    test_name text, query_text text, memory_limit text,
    repetitions integer DEFAULT 11)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    execution_order boolean[];
    batch_enabled boolean;
    started_at timestamptz;
BEGIN
    FOREACH batch_enabled IN ARRAY ARRAY[false, true] LOOP
        FOR warmup IN 1..3 LOOP
            PERFORM set_config('work_mem', memory_limit, false);
            PERFORM set_config('pg_batch.enable',
                               CASE WHEN batch_enabled
                                   THEN 'on' ELSE 'off' END, false);
            EXECUTE query_text;
        END LOOP;
    END LOOP;

    FOR sample IN 1..repetitions LOOP
        execution_order := CASE sample % 2
            WHEN 0 THEN ARRAY[false, true]
            ELSE ARRAY[true, false]
        END;
        FOREACH batch_enabled IN ARRAY execution_order LOOP
            PERFORM set_config('work_mem', memory_limit, false);
            PERFORM set_config('pg_batch.enable',
                               CASE WHEN batch_enabled
                                   THEN 'on' ELSE 'off' END, false);
            started_at := clock_timestamp();
            EXECUTE query_text;
            INSERT INTO pg_batch_group_aggregate_timings
            VALUES (test_name,
                    CASE WHEN batch_enabled THEN 'batch' ELSE 'postgres' END,
                    sample,
                    1000 * extract(epoch FROM clock_timestamp() - started_at));
        END LOOP;
    END LOOP;
END
$function$;

SELECT pg_temp.pg_batch_measure_group_aggregate(
    '1000 groups, in memory',
    $query$SELECT small_key, count(*), count(value), sum(value),
                   min(value), max(value)
            FROM pg_batch_group_aggregate
            GROUP BY small_key$query$,
    '64MB');

SELECT pg_temp.pg_batch_measure_group_aggregate(
    '200000 groups, in memory',
    $query$SELECT large_key, count(*), count(value), sum(value),
                   min(value), max(value)
            FROM pg_batch_group_aggregate
            GROUP BY large_key$query$,
    '64MB');

SELECT pg_temp.pg_batch_measure_group_aggregate(
    '200000 groups, spill',
    $query$SELECT large_key, count(*), count(value), sum(value),
                   min(value), max(value)
            FROM pg_batch_group_aggregate
            GROUP BY large_key$query$,
    '1MB');

WITH medians AS
(
    SELECT test,
           percentile_cont(0.5) WITHIN GROUP (ORDER BY milliseconds)
               FILTER (WHERE executor = 'postgres') AS postgres_ms,
           percentile_cont(0.5) WITHIN GROUP (ORDER BY milliseconds)
               FILTER (WHERE executor = 'batch') AS batch_ms
    FROM pg_batch_group_aggregate_timings
    GROUP BY test
)
SELECT test,
       round(postgres_ms::numeric, 3) AS postgres_ms,
       round(batch_ms::numeric, 3) AS batch_ms,
       round((postgres_ms / batch_ms)::numeric, 2) AS speedup
FROM medians
ORDER BY test;
