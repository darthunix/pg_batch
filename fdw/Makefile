MODULE_big = pg_batch_fdw
OBJS = \
	module.o \
	planner.o \
	scan.o \
	export.o

EXTENSION = pg_batch_fdw
DATA = pg_batch_fdw--0.1.sql

NANOARROW_PREFIX = $(abspath $(srcdir)/../build/nanoarrow-install)
PG_CPPFLAGS = \
	-I$(srcdir)/../bridge/include \
	-I$(srcdir)/../runtime/include \
	-I$(srcdir)/../kernels/include \
	-I$(NANOARROW_PREFIX)/include
SHLIB_LINK = \
	$(NANOARROW_PREFIX)/lib/libnanoarrow_ipc_static.a \
	$(NANOARROW_PREFIX)/lib/libnanoarrow_static.a \
	$(NANOARROW_PREFIX)/lib/libflatccrt.a
PG_BATCH_RUNTIME_LIB ?= $(abspath $(srcdir)/../build/meson-libs/runtime/libpg_batch_runtime.a)
PG_BATCH_KERNELS_LIB ?= $(abspath $(srcdir)/../build/meson-libs/kernels/libpg_batch_kernels.a)
SHLIB_LINK += $(PG_BATCH_KERNELS_LIB) $(PG_BATCH_RUNTIME_LIB)

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

$(MODULE_big)$(DLSUFFIX): $(PG_BATCH_KERNELS_LIB) $(PG_BATCH_RUNTIME_LIB)
