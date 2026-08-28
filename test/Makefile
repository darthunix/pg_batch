PG_CONFIG ?= pg_config

MODULES = pg_batch_bridge_test pg_batch_kernels_test
REGRESS = pg_batch pg_batch_fdw
REGRESS_OPTS = --dlpath=$(CURDIR)
PG_PKGLIBDIR := $(shell $(PG_CONFIG) --pkglibdir)
PG_BATCH_PKG_CONFIG_PATH = $(PG_PKGLIBDIR)/pkgconfig
PG_CPPFLAGS = $(shell PKG_CONFIG_PATH=$(PG_BATCH_PKG_CONFIG_PATH) \
		pkg-config --cflags pg_batch-kernels 2>/dev/null)
PG_LDFLAGS = $(shell PKG_CONFIG_PATH=$(PG_BATCH_PKG_CONFIG_PATH) \
	pkg-config --libs --static pg_batch-kernels 2>/dev/null)
NO_INSTALL = 1

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
