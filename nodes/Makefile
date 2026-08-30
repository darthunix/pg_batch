MODULE_big = pg_batch
OBJS = \
	module.o \
	planner.o \
	slot.o \
	executor.o \
	scan.o \
	filter.o \
	pack.o \
	project.o \
	hash_join.o \
	hash_join_spill.o \
	aggregate.o

EXTENSION = pg_batch
DATA = pg_batch--0.1.sql
PG_CPPFLAGS = \
	-I$(srcdir)/../include/pg_batch
PG_BATCH_RUNTIME_LIB ?= $(abspath $(srcdir)/../build/meson-libs/runtime/libpg_batch_runtime.a)
PG_BATCH_KERNELS_LIB ?= $(abspath $(srcdir)/../build/meson-libs/kernels/libpg_batch_kernels.a)
PG_BATCH_EXPR_LIB ?= $(abspath $(srcdir)/../build/meson-libs/expr/libpg_batch_expr.a)
PG_BATCH_SPILL_LIB ?= $(abspath $(srcdir)/../build/meson-libs/spill/libpg_batch_spill.a)
SHLIB_LINK = $(PG_BATCH_SPILL_LIB) $(PG_BATCH_EXPR_LIB) $(PG_BATCH_KERNELS_LIB) $(PG_BATCH_RUNTIME_LIB)

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

$(OBJS): $(wildcard $(srcdir)/*.h) \
	$(wildcard $(srcdir)/../include/pg_batch/*.h)
$(MODULE_big)$(DLSUFFIX): $(PG_BATCH_SPILL_LIB) $(PG_BATCH_EXPR_LIB) $(PG_BATCH_KERNELS_LIB) $(PG_BATCH_RUNTIME_LIB)
