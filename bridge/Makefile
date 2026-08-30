MODULE_big = pg_batch_api
OBJS = bridge.o
PG_CPPFLAGS = -I$(srcdir)/../include/pg_batch

EXTENSION = pg_batch_api
DATA = pg_batch_api--0.1.sql
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

$(OBJS): $(wildcard $(srcdir)/*.h) \
	$(wildcard $(srcdir)/../include/pg_batch/*.h)
