SUBDIRS = bridge nodes tam
PG_CONFIG ?= pg_config

.PHONY: all clean install uninstall installcheck $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@ PG_CONFIG="$(PG_CONFIG)"

clean:
	@for dir in $(SUBDIRS) test; do \
		$(MAKE) -C $$dir PG_CONFIG="$(PG_CONFIG)" clean || exit; \
	done

install:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir PG_CONFIG="$(PG_CONFIG)" install || exit; \
	done

uninstall:
	@for dir in tam nodes bridge; do \
		$(MAKE) -C $$dir PG_CONFIG="$(PG_CONFIG)" uninstall || exit; \
	done

installcheck:
	$(MAKE) -C test PG_CONFIG="$(PG_CONFIG)" all
	$(MAKE) -C test PG_CONFIG="$(PG_CONFIG)" installcheck
