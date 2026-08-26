MODULE_big = pg_batch_tam
OBJS = \
	module.o \
	provider.o \
	compressed.o \
	tableam.o

EXTENSION = pg_batch_tam
DATA = pg_batch_tam--0.1.sql
PG_CPPFLAGS = -I$(srcdir)/../bridge/include

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
