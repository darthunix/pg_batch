runtime_lib = static_library(
  'pg_batch_runtime',
  'adapter.c',
  dependencies: postgres_dep,
  include_directories: [bridge_inc, runtime_inc],
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: pg_pkglibdir,
  pic: true,
)

runtime_dep = declare_dependency(
  link_with: runtime_lib,
  dependencies: postgres_dep,
  include_directories: [bridge_inc, runtime_inc],
)

install_headers(
  'include/runtime.h',
  'include/vector.h',
  install_dir: pg_server_include / 'extension' / 'pg_batch_runtime',
)
