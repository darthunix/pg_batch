MODULE_big = pg_batch_bridge
OBJS = bridge.o
PG_CPPFLAGS = -I$(srcdir)/include

EXTENSION = pg_batch_bridge
DATA = pg_batch_bridge--0.1.sql
HEADERS_pg_batch_bridge = \
	include/bridge.h \
	include/arrow.h

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
