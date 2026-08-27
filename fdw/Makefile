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
	-I$(NANOARROW_PREFIX)/include
SHLIB_LINK = \
	$(NANOARROW_PREFIX)/lib/libnanoarrow_ipc_static.a \
	$(NANOARROW_PREFIX)/lib/libnanoarrow_static.a \
	$(NANOARROW_PREFIX)/lib/libflatccrt.a

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
