SUBDIRS = bridge nodes tam fdw
PG_CONFIG ?= pg_config
MESON_BUILD_DIR = $(CURDIR)/build/meson-libs
PG_BATCH_RUNTIME_LIB = $(MESON_BUILD_DIR)/runtime/libpg_batch_runtime.a
PG_BATCH_KERNELS_LIB = $(MESON_BUILD_DIR)/kernels/libpg_batch_kernels.a
PG_PKGLIBDIR = $(shell $(PG_CONFIG) --pkglibdir)
PG_INCLUDEDIR_SERVER = $(shell $(PG_CONFIG) --includedir-server)
NANOARROW_SOURCE = $(CURDIR)/third_party/nanoarrow
NANOARROW_BUILD = $(CURDIR)/build/nanoarrow
NANOARROW_PREFIX = $(CURDIR)/build/nanoarrow-install
NANOARROW_STAMP = $(NANOARROW_PREFIX)/.installed

.PHONY: all clean install uninstall installcheck libraries nanoarrow $(SUBDIRS)

all: nanoarrow libraries $(SUBDIRS)

libraries:
	@command -v meson >/dev/null || { \
		echo "meson is required to build pg_batch runtime libraries"; \
		exit 1; \
	}
	@if test -f "$(MESON_BUILD_DIR)/build.ninja"; then \
		CC="$$($(PG_CONFIG) --cc)" meson setup --reconfigure \
			"$(MESON_BUILD_DIR)" -Dpg_config="$(PG_CONFIG)"; \
	else \
		CC="$$($(PG_CONFIG) --cc)" meson setup \
			"$(MESON_BUILD_DIR)" -Dpg_config="$(PG_CONFIG)"; \
	fi
	meson compile -C "$(MESON_BUILD_DIR)"

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

nodes tam fdw: libraries
fdw: nanoarrow

$(SUBDIRS):
	$(MAKE) -C $@ PG_CONFIG="$(PG_CONFIG)" \
		PG_BATCH_RUNTIME_LIB="$(PG_BATCH_RUNTIME_LIB)" \
		PG_BATCH_KERNELS_LIB="$(PG_BATCH_KERNELS_LIB)"

clean:
	@for dir in $(SUBDIRS) test; do \
		$(MAKE) -C $$dir PG_CONFIG="$(PG_CONFIG)" clean || exit; \
	done
	@if test -f "$(MESON_BUILD_DIR)/build.ninja"; then \
		meson compile -C "$(MESON_BUILD_DIR)" --clean; \
	fi

install: nanoarrow libraries
	$(MAKE) -C bridge PG_CONFIG="$(PG_CONFIG)" install
	DESTDIR="$(DESTDIR)" meson install -C "$(MESON_BUILD_DIR)"
	@for dir in nodes tam fdw; do \
		$(MAKE) -C $$dir PG_CONFIG="$(PG_CONFIG)" install || exit; \
	done

uninstall:
	@for dir in fdw tam nodes bridge; do \
		$(MAKE) -C $$dir PG_CONFIG="$(PG_CONFIG)" uninstall || exit; \
	done
	rm -f \
		"$(DESTDIR)$(PG_PKGLIBDIR)/libpg_batch_runtime.a" \
		"$(DESTDIR)$(PG_PKGLIBDIR)/libpg_batch_kernels.a" \
		"$(DESTDIR)$(PG_PKGLIBDIR)/pkgconfig/pg_batch-runtime.pc" \
		"$(DESTDIR)$(PG_PKGLIBDIR)/pkgconfig/pg_batch-kernels.pc" \
		"$(DESTDIR)$(PG_INCLUDEDIR_SERVER)/extension/pg_batch_runtime/runtime.h" \
		"$(DESTDIR)$(PG_INCLUDEDIR_SERVER)/extension/pg_batch_runtime/vector.h" \
		"$(DESTDIR)$(PG_INCLUDEDIR_SERVER)/extension/pg_batch_kernels/kernels.h"

installcheck:
	$(MAKE) -C test PG_CONFIG="$(PG_CONFIG)" all
	$(MAKE) -C test PG_CONFIG="$(PG_CONFIG)" installcheck
