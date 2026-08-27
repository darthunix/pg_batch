SUBDIRS = bridge nodes tam fdw
PG_CONFIG ?= pg_config
NANOARROW_SOURCE = $(CURDIR)/third_party/nanoarrow
NANOARROW_BUILD = $(CURDIR)/build/nanoarrow
NANOARROW_PREFIX = $(CURDIR)/build/nanoarrow-install
NANOARROW_STAMP = $(NANOARROW_PREFIX)/.installed

.PHONY: all clean install uninstall installcheck nanoarrow $(SUBDIRS)

all: nanoarrow $(SUBDIRS)

nanoarrow: $(NANOARROW_STAMP)

$(NANOARROW_STAMP): $(NANOARROW_SOURCE)/CMakeLists.txt
	@mkdir -p "$(NANOARROW_BUILD)" "$(NANOARROW_PREFIX)"
	cmake -S "$(NANOARROW_SOURCE)" -B "$(NANOARROW_BUILD)" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$(NANOARROW_PREFIX)" \
		-DBUILD_SHARED_LIBS=OFF \
		-DNANOARROW_INSTALL_SHARED=OFF \
		-DNANOARROW_IPC=ON \
		-DNANOARROW_IPC_WITH_ZSTD=OFF \
		-DNANOARROW_IPC_WITH_LZ4=OFF \
		-DNANOARROW_BUILD_TESTS=OFF \
		-DNANOARROW_BUILD_INTEGRATION_TESTS=OFF \
		-DNANOARROW_BUILD_BENCHMARKS=OFF \
		-DNANOARROW_BUILD_APPS=OFF
	cmake --build "$(NANOARROW_BUILD)" --target nanoarrow_ipc_static
	cmake --install "$(NANOARROW_BUILD)"
	@touch "$@"

fdw: nanoarrow

$(SUBDIRS):
	$(MAKE) -C $@ PG_CONFIG="$(PG_CONFIG)"

clean:
	@for dir in $(SUBDIRS) test; do \
		$(MAKE) -C $$dir PG_CONFIG="$(PG_CONFIG)" clean || exit; \
	done

install: nanoarrow
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir PG_CONFIG="$(PG_CONFIG)" install || exit; \
	done

uninstall:
	@for dir in fdw tam nodes bridge; do \
		$(MAKE) -C $$dir PG_CONFIG="$(PG_CONFIG)" uninstall || exit; \
	done

installcheck:
	$(MAKE) -C test PG_CONFIG="$(PG_CONFIG)" all
	$(MAKE) -C test PG_CONFIG="$(PG_CONFIG)" installcheck
