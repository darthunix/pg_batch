CREATE FUNCTION pg_batch_compress(regclass)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pg_batch_compress'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION pg_batch_compress(regclass, integer)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pg_batch_compress'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION pg_batch_tableam_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'pg_batch_tableam_handler'
LANGUAGE C;

CREATE ACCESS METHOD pg_batch_compressed
TYPE TABLE
HANDLER pg_batch_tableam_handler;
