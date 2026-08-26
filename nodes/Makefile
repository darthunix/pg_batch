MODULE_big = pg_batch
OBJS = \
	module.o \
	planner.o \
	slot.o \
	executor.o \
	scan.o \
	filter.o \
	aggregate.o

EXTENSION = pg_batch
DATA = pg_batch--0.1.sql
PG_CPPFLAGS = -I$(srcdir)/../bridge/include

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
