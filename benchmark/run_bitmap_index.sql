\set ON_ERROR_STOP on

LOAD 'pg_batch';
SET max_parallel_workers_per_gather = 0;
SET jit = off;

BEGIN;
-- Isolate B-tree behavior from the BRIN indexes created by setup.sql.
DROP INDEX IF EXISTS pg_batch_bench_narrow_c1_brin;
DROP INDEX IF EXISTS pg_batch_bench_wide_c1_brin;
DROP INDEX IF EXISTS pg_batch_bench_plain_c1_brin;

CREATE INDEX IF NOT EXISTS pg_batch_bench_narrow_c1_btree
ON pg_batch_bench_narrow(c1);
CREATE INDEX IF NOT EXISTS pg_batch_bench_wide_c1_btree
ON pg_batch_bench_wide(c1);
CREATE INDEX IF NOT EXISTS pg_batch_bench_plain_c1_btree
ON pg_batch_bench_plain(c1);
CREATE INDEX IF NOT EXISTS pg_batch_bench_plain_c2_btree
ON pg_batch_bench_plain(c2);
ANALYZE pg_batch_bench_narrow;
ANALYZE pg_batch_bench_wide;
ANALYZE pg_batch_bench_plain;

CREATE TEMP TABLE pg_batch_bitmap_timings
(
    test text,
    executor text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_batch_set_bitmap_executor(executor_name text)
RETURNS void
LANGUAGE plpgsql
AS $function$
BEGIN
    PERFORM set_config('pg_batch.enable',
                       CASE WHEN executor_name LIKE 'batch_%' OR
                                 executor_name = 'auto_bitmap'
                            THEN 'on' ELSE 'off' END, false);
    PERFORM set_config('pg_batch.bitmap_min_rows_per_page',
                       CASE WHEN executor_name = 'batch_bitmap'
                            THEN '0' ELSE '8' END, false);
    PERFORM set_config('enable_seqscan',
                       CASE WHEN executor_name LIKE '%_seq'
                            THEN 'on' ELSE 'off' END, false);
    PERFORM set_config('enable_indexscan',
                       CASE WHEN executor_name = 'standard_index'
                            THEN 'on' ELSE 'off' END, false);
    PERFORM set_config('enable_indexonlyscan', 'off', false);
    PERFORM set_config('enable_bitmapscan',
                       CASE WHEN executor_name LIKE '%_bitmap'
                            THEN 'on' ELSE 'off' END, false);
END
$function$;

CREATE FUNCTION pg_batch_measure_bitmap(test_name text,
                                        statement_prefix text,
                                        arg1 integer,
                                        arg2 integer,
                                        arg3 integer DEFAULT 0,
                                        arg4 integer DEFAULT 0,
                                        repetitions integer DEFAULT 15)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    executors constant text[] := ARRAY[
        'standard_seq', 'batch_seq', 'standard_index',
        'standard_bitmap', 'auto_bitmap', 'batch_bitmap'
    ];
    executor_count constant integer := cardinality(executors);
    execution_order text[];
    executor_name text;
    previous_cpu_operator_cost text := current_setting('cpu_operator_cost');
    query_sql text;
    statement_name text;
    started_at timestamptz;
BEGIN
    CASE statement_prefix
        WHEN 'ordered_range' THEN
            query_sql := format(
                'SELECT count(*), sum(c4) FROM pg_batch_bench_narrow '
                'WHERE c1 BETWEEN %s AND %s', arg1, arg2);
        WHEN 'random_range' THEN
            query_sql := format(
                'SELECT count(*), sum(c4) FROM pg_batch_bench_plain '
                'WHERE c1 BETWEEN %s AND %s', arg1, arg2);
        WHEN 'bitmap_and' THEN
            query_sql := format(
                'SELECT count(*), sum(c4) FROM pg_batch_bench_plain '
                'WHERE c1 BETWEEN %s AND %s AND c2 BETWEEN %s AND %s',
                arg1, arg2, arg3, arg4);
        WHEN 'late_projection' THEN
            query_sql := format(
                'SELECT sum(c60) FROM pg_batch_bench_wide '
                'WHERE c1 BETWEEN %s AND %s AND c2 %% 7 = 0', arg1, arg2);
        ELSE
            RAISE EXCEPTION 'unknown bitmap statement prefix: %',
                            statement_prefix;
    END CASE;

    FOREACH executor_name IN ARRAY executors LOOP
        PERFORM pg_batch_set_bitmap_executor(executor_name);
        IF statement_prefix = 'bitmap_and' THEN
            -- Make both bitmap inputs cheaper than checking 100,000 heap rows.
            PERFORM set_config('cpu_operator_cost', '0.01', false);
        END IF;
        statement_name := test_name || '_' || executor_name;
        EXECUTE format('PREPARE %I AS %s', statement_name, query_sql);
        FOR warmup IN 1..3 LOOP
            EXECUTE format('EXECUTE %I', statement_name);
        END LOOP;
        PERFORM set_config('cpu_operator_cost', previous_cpu_operator_cost,
                           false);
    END LOOP;

    FOR sample IN 1..repetitions LOOP
        SELECT array_agg(executors[((i + sample - 2) % executor_count) + 1]
                         ORDER BY i)
          INTO execution_order
          FROM generate_series(1, executor_count) AS i;
        FOREACH executor_name IN ARRAY execution_order LOOP
            PERFORM pg_batch_set_bitmap_executor(executor_name);
            statement_name := test_name || '_' || executor_name;
            started_at := clock_timestamp();
            EXECUTE format('EXECUTE %I', statement_name);
            INSERT INTO pg_batch_bitmap_timings
            VALUES (test_name, executor_name, sample,
                    1000 * extract(epoch FROM clock_timestamp() - started_at));
        END LOOP;
    END LOOP;
END
$function$;

-- Ordered heap: exact B-tree matches remain concentrated on few pages.
SELECT pg_batch_measure_bitmap('ordered_0.01pct', 'ordered_range',
                               1000001, 1000200);
SELECT pg_batch_measure_bitmap('ordered_0.1pct', 'ordered_range',
                               1000001, 1002000);
SELECT pg_batch_measure_bitmap('ordered_1pct', 'ordered_range',
                               1000001, 1020000);
SELECT pg_batch_measure_bitmap('ordered_10pct', 'ordered_range',
                               1000001, 1200000);
SELECT pg_batch_measure_bitmap('ordered_50pct', 'ordered_range',
                               1000001, 2000000);

-- Random heap: the same selectivities spread matches across many pages.
SELECT pg_batch_measure_bitmap('random_0.01pct', 'random_range',
                               0, 214748);
SELECT pg_batch_measure_bitmap('random_0.1pct', 'random_range',
                               0, 2147483);
SELECT pg_batch_measure_bitmap('random_1pct', 'random_range',
                               0, 21474836);
SELECT pg_batch_measure_bitmap('random_2pct', 'random_range',
                               0, 42949672);
SELECT pg_batch_measure_bitmap('random_3pct', 'random_range',
                               0, 64424509);
SELECT pg_batch_measure_bitmap('random_5pct', 'random_range',
                               0, 107374182);
SELECT pg_batch_measure_bitmap('random_7.5pct', 'random_range',
                               0, 161061273);
SELECT pg_batch_measure_bitmap('random_10pct', 'random_range',
                               0, 214748364);
SELECT pg_batch_measure_bitmap('random_50pct', 'random_range',
                               0, 1073741823);

-- Two independent indexes select about one percent of the random heap.
SELECT pg_batch_measure_bitmap('random_bitmap_and', 'bitmap_and',
                               0, 214748364, 0, 214748364);

-- The residual expression runs before the late c60 projection is requested.
SELECT pg_batch_measure_bitmap('wide_late_projection', 'late_projection',
                               100001, 150000);

SELECT test,
       executor,
       round(percentile_cont(0.5) WITHIN GROUP (ORDER BY milliseconds)::numeric,
             3) AS median_ms
FROM pg_batch_bitmap_timings
GROUP BY test, executor
ORDER BY array_position(ARRAY[
             'ordered_0.01pct', 'ordered_0.1pct', 'ordered_1pct',
             'ordered_10pct', 'ordered_50pct', 'random_0.01pct',
             'random_0.1pct', 'random_1pct', 'random_2pct',
             'random_3pct', 'random_5pct', 'random_7.5pct',
             'random_10pct', 'random_50pct', 'random_bitmap_and',
             'wide_late_projection'
         ], test),
         array_position(ARRAY[
             'standard_seq', 'batch_seq', 'standard_index',
             'standard_bitmap', 'auto_bitmap', 'batch_bitmap'
         ], executor);

SELECT pg_batch_set_bitmap_executor('standard_index');
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE "ordered_0.01pct_standard_index";

SELECT pg_batch_set_bitmap_executor('standard_bitmap');
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE "random_1pct_standard_bitmap";

SELECT pg_batch_set_bitmap_executor('auto_bitmap');
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE "random_1pct_auto_bitmap";
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE "random_3pct_auto_bitmap";
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE "random_5pct_auto_bitmap";
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE "random_10pct_auto_bitmap";
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE wide_late_projection_auto_bitmap;

SELECT pg_batch_set_bitmap_executor('batch_bitmap');
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE ordered_1pct_batch_bitmap;
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE "random_1pct_batch_bitmap";
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE random_bitmap_and_batch_bitmap;
EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE wide_late_projection_batch_bitmap;

DROP FUNCTION pg_batch_measure_bitmap(text, text, integer, integer,
                                      integer, integer, integer);
DROP FUNCTION pg_batch_set_bitmap_executor(text);
DEALLOCATE ALL;
ROLLBACK;
