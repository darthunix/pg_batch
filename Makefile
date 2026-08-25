MODULE_big = pg_batch
OBJS = \
	pg_batch.o \
	pg_batch_plan.o \
	pg_batch_slot.o \
	pg_batch_exec.o \
	pg_batch_compress.o \
	pg_batch_tableam.o \
	pg_batch_scan.o \
	pg_batch_filter.o \
	pg_batch_agg.o

EXTENSION = pg_batch
DATA = pg_batch--0.1.sql
REGRESS = pg_batch

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
