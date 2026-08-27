MODULES = pg_batch_bridge_test
REGRESS = pg_batch pg_batch_fdw
REGRESS_OPTS = --dlpath=$(CURDIR)
PG_CPPFLAGS = -I$(srcdir)/../bridge/include
NO_INSTALL = 1

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
