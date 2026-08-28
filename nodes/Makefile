MODULE_big = pg_batch
OBJS = \
	module.o \
	planner.o \
	slot.o \
	executor.o \
	scan.o \
	filter.o \
	pack.o \
	hash_join.o \
	hash_join_spill.o \
	aggregate.o \
	../kernels/int4.o \
	../kernels/int4_simd.o

EXTENSION = pg_batch
DATA = pg_batch--0.1.sql
PG_CPPFLAGS = \
	-I$(srcdir)/../bridge/include \
	-I$(srcdir)/../runtime/include \
	-I$(srcdir)/../kernels/include

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
