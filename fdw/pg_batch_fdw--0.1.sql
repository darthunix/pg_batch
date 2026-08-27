CREATE FUNCTION pg_batch_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME', 'pg_batch_fdw_handler'
LANGUAGE C STRICT;

CREATE FUNCTION pg_batch_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_batch_fdw_validator'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER pg_batch_fdw
HANDLER pg_batch_fdw_handler
VALIDATOR pg_batch_fdw_validator;

CREATE FUNCTION pg_batch_fdw_export(regclass, text, integer DEFAULT 4096)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pg_batch_fdw_export'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION pg_batch_fdw_remove(text)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_batch_fdw_remove'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

REVOKE ALL ON FUNCTION pg_batch_fdw_export(regclass, text, integer) FROM PUBLIC;
REVOKE ALL ON FUNCTION pg_batch_fdw_remove(text) FROM PUBLIC;
