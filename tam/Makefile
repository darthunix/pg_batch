MODULE_big = pg_batch_tam
OBJS = \
	module.o \
	provider.o \
	compressed.o \
	tableam.o

EXTENSION = pg_batch_tam
DATA = pg_batch_tam--0.1.sql
PG_CPPFLAGS = \
	-I$(srcdir)/../bridge/include \
	-I$(srcdir)/../runtime/include \
	-I$(srcdir)/../kernels/include
PG_BATCH_RUNTIME_LIB ?= $(abspath $(srcdir)/../build/meson-libs/runtime/libpg_batch_runtime.a)
PG_BATCH_KERNELS_LIB ?= $(abspath $(srcdir)/../build/meson-libs/kernels/libpg_batch_kernels.a)
SHLIB_LINK = $(PG_BATCH_KERNELS_LIB) $(PG_BATCH_RUNTIME_LIB)

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

$(MODULE_big)$(DLSUFFIX): $(PG_BATCH_KERNELS_LIB) $(PG_BATCH_RUNTIME_LIB)
