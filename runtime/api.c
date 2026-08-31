#include "postgres.h"

#include "fmgr.h"

#include "runtime.h"

const PgBatchAPI *
pg_batch_api_get(void)
{
	void	  **rendezvous = find_rendezvous_variable(PG_BATCH_RENDEZVOUS);
	const PgBatchAPI *api;

	if (*rendezvous == NULL)
		load_file("$libdir/pg_batch_api", false);
	api = *rendezvous;
	if (api == NULL || api->abi_version != PG_BATCH_API_ABI_VERSION ||
		api->struct_size < PG_BATCH_API_MIN_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("incompatible pg_batch_api library")));
	return api;
}
