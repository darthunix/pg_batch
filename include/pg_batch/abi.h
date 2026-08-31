/* Helpers for append-only C interfaces shared by independent extensions. */
#ifndef PG_BATCH_ABI_H
#define PG_BATCH_ABI_H

#include "postgres.h"

/* Size of a public structure through one required field. */
#define PG_BATCH_ABI_SIZE_THROUGH(type, field) \
	(offsetof(type, field) + sizeof(((type *) 0)->field))

/* True when an append-only structure supplied by another module has field. */
#define PG_BATCH_ABI_HAS_FIELD(object, type, field) \
	((object) != NULL && (object)->struct_size >= \
	 PG_BATCH_ABI_SIZE_THROUGH(type, field))

/* Header for a versioned operation table defined with designated fields. */
#define PG_BATCH_ABI_INITIALIZER(version, type) \
	.abi_version = (version), .struct_size = sizeof(type)

/* Initial value for an append-only callback argument or result. */
#define PG_BATCH_STRUCT_INITIALIZER(type) \
	{.struct_size = sizeof(type)}

#endif /* PG_BATCH_ABI_H */
