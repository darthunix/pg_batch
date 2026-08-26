\set ON_ERROR_STOP on

LOAD 'pg_batch_tam';
LOAD 'pg_batch';
SET max_parallel_workers_per_gather = 0;
SET jit = off;

DROP TABLE IF EXISTS pg_batch_bench_heap_groups;
CREATE TABLE pg_batch_bench_heap_groups AS
SELECT c1, c2, c3, c4, (c1 % 8) * 2 AS small_set
FROM pg_batch_bench_narrow;
CREATE INDEX pg_batch_bench_heap_groups_c1_brin
ON pg_batch_bench_heap_groups USING brin(c1) WITH (pages_per_range = 32);
CREATE INDEX pg_batch_bench_heap_groups_small_set_brin
ON pg_batch_bench_heap_groups USING brin(small_set int4_bloom_ops)
WITH (pages_per_range = 32);
VACUUM (ANALYZE) pg_batch_bench_heap_groups;

DROP TABLE IF EXISTS pg_batch_bench_tam;
CREATE TABLE pg_batch_bench_tam
(
    c1 int,
    c2 int,
    c3 int,
    c4 int,
    small_set int
)
USING pg_batch_compressed;
INSERT INTO pg_batch_bench_tam SELECT * FROM pg_batch_bench_heap_groups;
VACUUM (ANALYZE) pg_batch_bench_tam;

SELECT pg_batch_compress('pg_batch_bench_tam', 4096) AS compressed_bytes;

PREPARE seq_range_hit AS
SELECT count(*) FROM pg_batch_bench_heap_groups
WHERE c1 BETWEEN 1000001 AND 1001000;
PREPARE heap_batch_range_hit AS
SELECT count(*) FROM pg_batch_bench_heap_groups
WHERE c1 BETWEEN 1000001 AND 1001000;
PREPARE brin_range_hit AS
SELECT count(*) FROM pg_batch_bench_heap_groups
WHERE c1 BETWEEN 1000001 AND 1001000;
PREPARE batch_brin_range_hit AS
SELECT count(*) FROM pg_batch_bench_heap_groups
WHERE c1 BETWEEN 1000001 AND 1001000;

-- The provider captures scan_mode while planning. Keep one prepared statement
-- per mode so the plan cache cannot make all three measurements use one mode.
SET pg_batch_tam.scan_mode = batch;
PREPARE arrow_batch_range_hit AS
SELECT count(*) FROM pg_batch_bench_tam
WHERE c1 BETWEEN 1000001 AND 1001000;

PREPARE seq_range_miss AS
SELECT count(*) FROM pg_batch_bench_heap_groups WHERE c1 < 0;
PREPARE heap_batch_range_miss AS
SELECT count(*) FROM pg_batch_bench_heap_groups WHERE c1 < 0;
PREPARE brin_range_miss AS
SELECT count(*) FROM pg_batch_bench_heap_groups WHERE c1 < 0;
PREPARE batch_brin_range_miss AS
SELECT count(*) FROM pg_batch_bench_heap_groups WHERE c1 < 0;
PREPARE arrow_batch_range_miss AS
SELECT count(*) FROM pg_batch_bench_tam WHERE c1 < 0;

PREPARE seq_membership_miss AS
SELECT count(*) FROM pg_batch_bench_heap_groups WHERE small_set = 7;
PREPARE heap_batch_membership_miss AS
SELECT count(*) FROM pg_batch_bench_heap_groups WHERE small_set = 7;
PREPARE brin_membership_miss AS
SELECT count(*) FROM pg_batch_bench_heap_groups WHERE small_set = 7;
PREPARE batch_brin_membership_miss AS
SELECT count(*) FROM pg_batch_bench_heap_groups WHERE small_set = 7;
PREPARE arrow_batch_membership_miss AS
SELECT count(*) FROM pg_batch_bench_tam WHERE small_set = 7;

PREPARE seq_dense AS
SELECT sum(c4) FROM pg_batch_bench_heap_groups
WHERE c1 < 1500000 AND small_set = 2;
PREPARE heap_batch_dense AS
SELECT sum(c4) FROM pg_batch_bench_heap_groups
WHERE c1 < 1500000 AND small_set = 2;
PREPARE brin_dense AS
SELECT sum(c4) FROM pg_batch_bench_heap_groups
WHERE c1 < 1500000 AND small_set = 2;
PREPARE batch_brin_dense AS
SELECT sum(c4) FROM pg_batch_bench_heap_groups
WHERE c1 < 1500000 AND small_set = 2;
PREPARE arrow_batch_dense AS
SELECT sum(c4) FROM pg_batch_bench_tam
WHERE c1 < 1500000 AND small_set = 2;

SET pg_batch_tam.scan_mode = prune;
PREPARE prune_range_hit AS
SELECT count(*) FROM pg_batch_bench_tam
WHERE c1 BETWEEN 1000001 AND 1001000;
PREPARE prune_range_miss AS
SELECT count(*) FROM pg_batch_bench_tam WHERE c1 < 0;
PREPARE prune_membership_miss AS
SELECT count(*) FROM pg_batch_bench_tam WHERE small_set = 7;
PREPARE prune_dense AS
SELECT sum(c4) FROM pg_batch_bench_tam
WHERE c1 < 1500000 AND small_set = 2;

SET pg_batch_tam.scan_mode = filter;
PREPARE filter_range_hit AS
SELECT count(*) FROM pg_batch_bench_tam
WHERE c1 BETWEEN 1000001 AND 1001000;
PREPARE filter_range_miss AS
SELECT count(*) FROM pg_batch_bench_tam WHERE c1 < 0;
PREPARE filter_membership_miss AS
SELECT count(*) FROM pg_batch_bench_tam WHERE small_set = 7;
PREPARE filter_dense AS
SELECT sum(c4) FROM pg_batch_bench_tam
WHERE c1 < 1500000 AND small_set = 2;

CREATE TEMP TABLE pg_batch_group_timings
(
    test text,
    executor text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_batch_set_group_executor(executor_name text)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
BEGIN
    PERFORM set_config('pg_batch.enable',
                       CASE WHEN executor_name IN ('postgres', 'brin')
                            THEN 'off' ELSE 'on' END, false);
    PERFORM set_config('enable_seqscan',
                       CASE WHEN executor_name IN ('brin', 'batch_brin')
                            THEN 'off' ELSE 'on' END, false);
    PERFORM set_config('enable_bitmapscan',
                       CASE WHEN executor_name IN ('brin', 'batch_brin')
                            THEN 'on' ELSE 'off' END, false);
    PERFORM set_config('pg_batch_tam.scan_mode',
                       CASE executor_name
                           WHEN 'arrow_batch' THEN 'batch'
                           WHEN 'prune' THEN 'prune'
                           ELSE 'filter'
                       END, false);
END
$function$;

CREATE FUNCTION pg_batch_measure_groups(test_name text,
                                        repetitions integer DEFAULT 15)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    executors constant text[] := ARRAY[
        'postgres', 'heap_batch', 'brin', 'batch_brin',
        'arrow_batch', 'prune', 'filter'];
    execution_order text[];
    executor_name text;
    statement_name text;
    started_at timestamptz;
BEGIN
    FOREACH executor_name IN ARRAY executors LOOP
        PERFORM pg_batch_set_group_executor(executor_name);
        statement_name := CASE
            WHEN executor_name = 'postgres'
                THEN 'seq_' || test_name
            ELSE executor_name || '_' || test_name
        END;
        FOR warmup IN 1..3 LOOP
            EXECUTE format('EXECUTE %I', statement_name);
        END LOOP;
    END LOOP;

    FOR sample IN 1..repetitions LOOP
        SELECT array_agg(executors[((i + sample - 2) % 7) + 1] ORDER BY i)
          INTO execution_order
          FROM generate_series(1, 7) AS i;
        FOREACH executor_name IN ARRAY execution_order LOOP
            PERFORM pg_batch_set_group_executor(executor_name);
            statement_name := CASE
                WHEN executor_name = 'postgres'
                    THEN 'seq_' || test_name
                ELSE executor_name || '_' || test_name
            END;
            started_at := clock_timestamp();
            EXECUTE format('EXECUTE %I', statement_name);
            INSERT INTO pg_batch_group_timings
            VALUES (replace(test_name, '_', ' '), executor_name, sample,
                    1000 * extract(epoch FROM clock_timestamp() - started_at));
        END LOOP;
    END LOOP;
END
$function$;

SELECT pg_batch_measure_groups('range_hit');
SELECT pg_batch_measure_groups('range_miss');
SELECT pg_batch_measure_groups('membership_miss');
SELECT pg_batch_measure_groups('dense');

SELECT test,
       executor,
       round(percentile_cont(0.5) WITHIN GROUP (ORDER BY milliseconds)::numeric,
             3) AS median_ms
FROM pg_batch_group_timings
GROUP BY test, executor
ORDER BY test,
         array_position(ARRAY[
             'postgres', 'heap_batch', 'brin', 'batch_brin',
             'arrow_batch', 'prune', 'filter'], executor);

SELECT pg_batch_set_group_executor('batch_brin');
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE batch_brin_range_hit;
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE batch_brin_membership_miss;

SELECT pg_batch_set_group_executor('filter');
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE filter_range_hit;
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE filter_membership_miss;

SELECT pg_batch_set_group_executor('arrow_batch');
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE arrow_batch_range_hit;

SELECT pg_batch_set_group_executor('prune');
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE prune_range_hit;

DROP FUNCTION pg_batch_measure_groups(text, integer);
DROP FUNCTION pg_batch_set_group_executor(text);
DEALLOCATE ALL;
