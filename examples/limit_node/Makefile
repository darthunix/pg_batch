MODULE_big = pg_batch_limit
OBJS = \
	module.o \
	planner.o \
	executor.o

EXTENSION = pg_batch_limit
DATA = pg_batch_limit--0.1.sql

ifdef PG_BATCH_RUNTIME_LIB
PG_CPPFLAGS = -I$(srcdir)/../../include/pg_batch
SHLIB_LINK = $(PG_BATCH_RUNTIME_LIB)
else
PG_BATCH_PKG_CONFIG_PATH = $(shell $(PG_CONFIG) --pkglibdir)/pkgconfig
PG_CPPFLAGS = $(shell PKG_CONFIG_PATH=$(PG_BATCH_PKG_CONFIG_PATH) \
	pkg-config --cflags pg_batch-runtime)
SHLIB_LINK = $(shell PKG_CONFIG_PATH=$(PG_BATCH_PKG_CONFIG_PATH) \
	pkg-config --libs --static pg_batch-runtime)
endif

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

$(OBJS): $(wildcard $(srcdir)/*.h) \
	$(wildcard $(srcdir)/../../include/pg_batch/*.h)
$(MODULE_big)$(DLSUFFIX): $(PG_BATCH_RUNTIME_LIB)
