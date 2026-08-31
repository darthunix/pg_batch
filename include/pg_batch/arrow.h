/*
 * Arrow C Data Interface definitions copied from the Apache Arrow format
 * specification. They are available under the Apache License 2.0.
 *
 * https://arrow.apache.org/docs/format/CDataInterface.html
 */
#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

#include <stdint.h>

#define ARROW_FLAG_DICTIONARY_ORDERED 1
#define ARROW_FLAG_NULLABLE 2
#define ARROW_FLAG_MAP_KEYS_SORTED 4

struct ArrowSchema
{
	const char *format;
	const char *name;
	const char *metadata;
	int64_t		flags;
	int64_t		n_children;
	struct ArrowSchema **children;
	struct ArrowSchema *dictionary;
	void		(*release) (struct ArrowSchema *);
	void	   *private_data;
};

struct ArrowArray
{
	int64_t		length;
	int64_t		null_count;
	int64_t		offset;
	int64_t		n_buffers;
	int64_t		n_children;
	const void **buffers;
	struct ArrowArray **children;
	struct ArrowArray *dictionary;
	void		(*release) (struct ArrowArray *);
	void	   *private_data;
};

#endif							/* ARROW_C_DATA_INTERFACE */

#ifndef PG_BATCH_ARROW_HELPERS
#define PG_BATCH_ARROW_HELPERS

static inline bool
pg_batch_arrow_row_is_valid(const struct ArrowArray *array, int row)
{
	const uint8 *validity = array->buffers[0];
	int64		position = array->offset + row;

	return validity == NULL ||
		(validity[position / 8] & ((uint8) 1 << (position % 8))) != 0;
}

#endif							/* PG_BATCH_ARROW_HELPERS */

#ifndef PG_BATCH_ARROW_INTERFACE
#define PG_BATCH_ARROW_INTERFACE

#include "batch.h"

#define PG_BATCH_ARROW_INTERFACE_NAME "pg_batch.arrow.c_data"
#define PG_BATCH_ARROW_INTERFACE_VERSION 1

typedef struct PgBatchArrowView
{
	Size		struct_size;
	const struct ArrowArray *array;
	const struct ArrowSchema *schema;
} PgBatchArrowView;

#define PG_BATCH_ARROW_VIEW_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchArrowView, schema)

extern const PgBatchNativeType pg_batch_arrow_type;

typedef struct PgBatchArrowInterface
{
	uint32		abi_version;
	Size		struct_size;
	void		(*get_column) (PgBatch *batch, int column,
							   PgBatchArrowView *result);
} PgBatchArrowInterface;

#define PG_BATCH_ARROW_INTERFACE_MIN_SIZE \
	PG_BATCH_ABI_SIZE_THROUGH(PgBatchArrowInterface, get_column)

#endif							/* PG_BATCH_ARROW_INTERFACE */
