\set ON_ERROR_STOP on

LOAD 'pg_batch';
SET max_parallel_workers_per_gather = 0;
SET jit = off;

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

INSERT INTO pg_batch_bench_tam
SELECT c1, c2, c3, c4, (c1 % 8) * 2
FROM pg_batch_bench_narrow;
VACUUM (ANALYZE) pg_batch_bench_tam;

SELECT pg_batch_compress('pg_batch_bench_tam', 4096) AS compressed_bytes;

SET pg_batch.enable = on;
SET pg_batch.use_compressed = on;
PREPARE smart_range_hit AS
SELECT count(*) FROM pg_batch_bench_tam
WHERE c1 BETWEEN 1000001 AND 1001000;
PREPARE smart_range_miss AS
SELECT count(*) FROM pg_batch_bench_tam WHERE c1 < 0;
PREPARE smart_membership_miss AS
SELECT count(*) FROM pg_batch_bench_tam WHERE small_set = 7;
PREPARE smart_dense AS
SELECT sum(c4) FROM pg_batch_bench_tam
WHERE c1 < 1500000 AND small_set = 2;

SET pg_batch.enable = off;
PREPARE plain_range_hit AS
SELECT count(*) FROM pg_batch_bench_tam
WHERE c1 BETWEEN 1000001 AND 1001000;
PREPARE plain_range_miss AS
SELECT count(*) FROM pg_batch_bench_tam WHERE c1 < 0;
PREPARE plain_membership_miss AS
SELECT count(*) FROM pg_batch_bench_tam WHERE small_set = 7;
PREPARE plain_dense AS
SELECT sum(c4) FROM pg_batch_bench_tam
WHERE c1 < 1500000 AND small_set = 2;

CREATE TEMP TABLE pg_batch_group_timings
(
    test text,
    executor text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_batch_measure_groups(test_name text,
                                        plain_statement text,
                                        smart_statement text,
                                        repetitions integer DEFAULT 15)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    executors constant text[] :=
        ARRAY['postgres', 'batch', 'prune', 'filter', 'tam_filter'];
    execution_order text[];
    executor_name text;
    statement_name text;
    started_at timestamptz;
BEGIN
    FOREACH executor_name IN ARRAY executors LOOP
        FOR warmup IN 1..3 LOOP
            PERFORM set_config('pg_batch.enable',
                               CASE WHEN executor_name = 'postgres'
                                    THEN 'off' ELSE 'on' END, false);
            PERFORM set_config('pg_batch.compressed_scan_mode',
                               CASE executor_name
                                   WHEN 'batch' THEN 'batch'
                                   WHEN 'prune' THEN 'prune'
                                   ELSE 'filter'
                               END, false);
            PERFORM set_config('pg_batch.compressed_via_tableam',
                               CASE WHEN executor_name = 'tam_filter'
                                    THEN 'on' ELSE 'off' END, false);
            statement_name := CASE WHEN executor_name = 'postgres'
                                   THEN plain_statement ELSE smart_statement END;
            EXECUTE format('EXECUTE %I', statement_name);
        END LOOP;
    END LOOP;

    FOR sample IN 1..repetitions LOOP
        SELECT array_agg(executors[((i + sample - 2) % 5) + 1] ORDER BY i)
          INTO execution_order
          FROM generate_series(1, 5) AS i;
        FOREACH executor_name IN ARRAY execution_order LOOP
            PERFORM set_config('pg_batch.enable',
                               CASE WHEN executor_name = 'postgres'
                                    THEN 'off' ELSE 'on' END, false);
            PERFORM set_config('pg_batch.compressed_scan_mode',
                               CASE executor_name
                                   WHEN 'batch' THEN 'batch'
                                   WHEN 'prune' THEN 'prune'
                                   ELSE 'filter'
                               END, false);
            PERFORM set_config('pg_batch.compressed_via_tableam',
                               CASE WHEN executor_name = 'tam_filter'
                                    THEN 'on' ELSE 'off' END, false);
            statement_name := CASE WHEN executor_name = 'postgres'
                                   THEN plain_statement ELSE smart_statement END;
            started_at := clock_timestamp();
            EXECUTE format('EXECUTE %I', statement_name);
            INSERT INTO pg_batch_group_timings
            VALUES (test_name, executor_name, sample,
                    1000 * extract(epoch FROM clock_timestamp() - started_at));
        END LOOP;
    END LOOP;
END
$function$;

SELECT pg_batch_measure_groups('range hit',
                               'plain_range_hit', 'smart_range_hit');
SELECT pg_batch_measure_groups('range miss',
                               'plain_range_miss', 'smart_range_miss');
SELECT pg_batch_measure_groups('membership miss',
                               'plain_membership_miss',
                               'smart_membership_miss');
SELECT pg_batch_measure_groups('dense filter and sum',
                               'plain_dense', 'smart_dense');

SELECT test,
       executor,
       round(percentile_cont(0.5) WITHIN GROUP (ORDER BY milliseconds)::numeric,
             3) AS median_ms
FROM pg_batch_group_timings
GROUP BY test, executor
ORDER BY test,
         array_position(ARRAY['postgres', 'batch', 'prune', 'filter',
                              'tam_filter'], executor);

SET pg_batch.enable = on;
SET pg_batch.compressed_scan_mode = filter;
SET pg_batch.compressed_via_tableam = on;
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE smart_range_hit;

DROP FUNCTION pg_batch_measure_groups(text, text, text, integer);
DEALLOCATE ALL;
