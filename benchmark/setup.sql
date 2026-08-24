\set ON_ERROR_STOP on

DROP TABLE IF EXISTS pg_batch_bench_narrow;
CREATE TABLE pg_batch_bench_narrow AS
SELECT g AS c1,
       g + 1 AS c2,
       g + 2 AS c3,
       g + 3 AS c4,
       g + 4 AS c5,
       g + 5 AS c6,
       g + 6 AS c7,
       g + 7 AS c8
FROM generate_series(1, 2000000) AS g;

DROP TABLE IF EXISTS pg_batch_bench_wide;
DO $do$
DECLARE
    columns text;
BEGIN
    SELECT string_agg(format('g + %s AS c%s', i, i), ', ')
      INTO columns
      FROM generate_series(1, 60) AS i;
    EXECUTE 'CREATE TABLE pg_batch_bench_wide AS SELECT ' || columns ||
            ' FROM generate_series(1, 250000) AS g';
END
$do$;

DROP TABLE IF EXISTS pg_batch_bench_mixed;
CREATE TABLE pg_batch_bench_mixed AS
SELECT g AS a,
       CASE WHEN g % 7 = 0 THEN NULL ELSE 'b-' || g END AS b,
       g::bigint AS c,
       g AS d,
       CASE WHEN g % 11 = 0 THEN NULL ELSE 'e-' || g END AS e,
       g::bigint AS f,
       g AS g,
       CASE WHEN g % 13 = 0 THEN NULL ELSE 'h-' || g END AS h,
       g::bigint AS i,
       g AS j,
       CASE WHEN g % 17 = 0 THEN NULL ELSE 'k-' || g END AS k,
       g::bigint AS l,
       g AS m,
       CASE WHEN g % 19 = 0 THEN NULL ELSE 'n-' || g END AS n,
       CASE WHEN g % 7 = 0 THEN NULL ELSE g::bigint END AS o,
       g AS p
FROM generate_series(1, 500000) AS g;

DROP TABLE IF EXISTS pg_batch_bench_plain;
CREATE TABLE pg_batch_bench_plain AS
SELECT ((g::bigint * 1103515245 + 12345) % 2147483647)::int AS c1,
       ((g::bigint * 214013 + 2531011) % 2147483647)::int AS c2,
       ((g::bigint * 48271 + 17) % 2147483647)::int AS c3,
       ((g::bigint * 16807 + 31) % 2147483647)::int AS c4
FROM generate_series(1, 1000000) AS g;

VACUUM (ANALYZE) pg_batch_bench_narrow;
VACUUM (ANALYZE) pg_batch_bench_wide;
VACUUM (ANALYZE) pg_batch_bench_mixed;
VACUUM (ANALYZE) pg_batch_bench_plain;

SELECT pg_size_pretty(pg_total_relation_size('pg_batch_bench_narrow')) AS narrow,
       pg_size_pretty(pg_total_relation_size('pg_batch_bench_wide')) AS wide,
       pg_size_pretty(pg_total_relation_size('pg_batch_bench_mixed')) AS mixed,
       pg_size_pretty(pg_total_relation_size('pg_batch_bench_plain')) AS plain;
