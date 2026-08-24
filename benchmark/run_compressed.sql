\set ON_ERROR_STOP on

LOAD 'pg_batch';
SET max_parallel_workers_per_gather = 0;
SET jit = off;

CREATE TEMP TABLE pg_batch_compression_stats
(
    relation_name text,
    logical_bytes bigint,
    compressed_bytes bigint,
    build_ms numeric
);

CREATE FUNCTION pg_batch_build_snapshot(relation_name regclass,
                                        source_rows bigint,
                                        source_columns integer)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    started_at timestamptz;
    bytes bigint;
BEGIN
    started_at := clock_timestamp();
    bytes := pg_batch_compress(relation_name);
    INSERT INTO pg_batch_compression_stats
    VALUES (relation_name::text,
            source_rows * source_columns * 4,
            bytes,
            round(1000 * extract(epoch FROM clock_timestamp() - started_at), 3));
END
$function$;

SELECT pg_batch_build_snapshot('pg_batch_bench_narrow', 2000000, 8);
SELECT pg_batch_build_snapshot('pg_batch_bench_wide', 250000, 60);
SELECT pg_batch_build_snapshot('pg_batch_bench_plain', 1000000, 4);

SET pg_batch.enable = on;
SET pg_batch.use_compressed = off;
PREPARE batch_narrow_dense AS
SELECT count(*) FROM pg_batch_bench_narrow
WHERE c1 > 100 AND c2 < 1500000;
PREPARE batch_narrow_none AS
SELECT count(*) FROM pg_batch_bench_narrow WHERE c1 < 0;
PREPARE batch_wide_late_projection AS
SELECT c60 FROM pg_batch_bench_wide WHERE c2 < 1000;
PREPARE batch_wide_early_projection AS
SELECT c2 FROM pg_batch_bench_wide WHERE c60 < 1058;
PREPARE batch_wide_count AS
SELECT count(*) FROM pg_batch_bench_wide WHERE c2 < 1000;
PREPARE batch_wide_sum AS
SELECT sum(c60) FROM pg_batch_bench_wide WHERE c2 < 1000;
PREPARE batch_plain_count AS
SELECT count(*) FROM pg_batch_bench_plain WHERE c1 < 1073741824;
PREPARE batch_plain_sum AS
SELECT sum(c4) FROM pg_batch_bench_plain WHERE c1 < 1073741824;

SET pg_batch.enable = off;
PREPARE plain_narrow_dense AS
SELECT count(*) FROM pg_batch_bench_narrow
WHERE c1 > 100 AND c2 < 1500000;
PREPARE plain_narrow_none AS
SELECT count(*) FROM pg_batch_bench_narrow WHERE c1 < 0;
PREPARE plain_wide_late_projection AS
SELECT c60 FROM pg_batch_bench_wide WHERE c2 < 1000;
PREPARE plain_wide_early_projection AS
SELECT c2 FROM pg_batch_bench_wide WHERE c60 < 1058;
PREPARE plain_wide_count AS
SELECT count(*) FROM pg_batch_bench_wide WHERE c2 < 1000;
PREPARE plain_wide_sum AS
SELECT sum(c60) FROM pg_batch_bench_wide WHERE c2 < 1000;
PREPARE plain_plain_count AS
SELECT count(*) FROM pg_batch_bench_plain WHERE c1 < 1073741824;
PREPARE plain_plain_sum AS
SELECT sum(c4) FROM pg_batch_bench_plain WHERE c1 < 1073741824;

CREATE TEMP TABLE pg_batch_timings
(
    test text,
    executor text,
    run integer,
    milliseconds numeric
);

CREATE FUNCTION pg_batch_measure(test_name text,
                                 plain_statement text,
                                 batch_statement text,
                                 repetitions integer DEFAULT 15)
RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    execution_order text[];
    executor_name text;
    statement_name text;
    started_at timestamptz;
BEGIN
    FOREACH executor_name IN ARRAY ARRAY['postgres', 'heap', 'arrow'] LOOP
        FOR warmup IN 1..3 LOOP
            PERFORM set_config('pg_batch.enable',
                               CASE WHEN executor_name = 'postgres'
                                    THEN 'off' ELSE 'on' END, false);
            PERFORM set_config('pg_batch.use_compressed',
                               CASE WHEN executor_name = 'arrow'
                                    THEN 'on' ELSE 'off' END, false);
            statement_name := CASE WHEN executor_name = 'postgres'
                                   THEN plain_statement ELSE batch_statement END;
            EXECUTE format('EXECUTE %I', statement_name);
        END LOOP;
    END LOOP;

    FOR sample IN 1..repetitions LOOP
        execution_order := CASE sample % 3
            WHEN 0 THEN ARRAY['postgres', 'heap', 'arrow']
            WHEN 1 THEN ARRAY['heap', 'arrow', 'postgres']
            ELSE ARRAY['arrow', 'postgres', 'heap']
        END;
        FOREACH executor_name IN ARRAY execution_order LOOP
            PERFORM set_config('pg_batch.enable',
                               CASE WHEN executor_name = 'postgres'
                                    THEN 'off' ELSE 'on' END, false);
            PERFORM set_config('pg_batch.use_compressed',
                               CASE WHEN executor_name = 'arrow'
                                    THEN 'on' ELSE 'off' END, false);
            statement_name := CASE WHEN executor_name = 'postgres'
                                   THEN plain_statement ELSE batch_statement END;
            started_at := clock_timestamp();
            EXECUTE format('EXECUTE %I', statement_name);
            INSERT INTO pg_batch_timings
            VALUES (test_name, executor_name, sample,
                    1000 * extract(epoch FROM clock_timestamp() - started_at));
        END LOOP;
    END LOOP;
END
$function$;

SELECT pg_batch_measure('narrow dense quals',
                        'plain_narrow_dense', 'batch_narrow_dense');
SELECT pg_batch_measure('narrow pass none',
                        'plain_narrow_none', 'batch_narrow_none');
SELECT pg_batch_measure('wide early filter late projection',
                        'plain_wide_late_projection',
                        'batch_wide_late_projection');
SELECT pg_batch_measure('wide late filter early projection',
                        'plain_wide_early_projection',
                        'batch_wide_early_projection');
SELECT pg_batch_measure('wide count without projection',
                        'plain_wide_count', 'batch_wide_count');
SELECT pg_batch_measure('wide sum from native batch',
                        'plain_wide_sum', 'batch_wide_sum');
SELECT pg_batch_measure('plain encoding count',
                        'plain_plain_count', 'batch_plain_count');
SELECT pg_batch_measure('delta16 encoding sum',
                        'plain_plain_sum', 'batch_plain_sum');

SELECT relation_name,
       pg_size_pretty(logical_bytes) AS logical_size,
       pg_size_pretty(compressed_bytes) AS compressed_size,
       round(100.0 * compressed_bytes / logical_bytes, 1) AS percent,
       build_ms
FROM pg_batch_compression_stats
ORDER BY relation_name;

SELECT test,
       executor,
       round(percentile_cont(0.5) WITHIN GROUP (ORDER BY milliseconds)::numeric,
             3) AS median_ms
FROM pg_batch_timings
GROUP BY test, executor
ORDER BY test,
         CASE executor
             WHEN 'postgres' THEN 1
             WHEN 'heap' THEN 2
             ELSE 3
         END;

DROP FUNCTION pg_batch_measure(text, text, text, integer);
DROP FUNCTION pg_batch_build_snapshot(regclass, bigint, integer);
DEALLOCATE ALL;
