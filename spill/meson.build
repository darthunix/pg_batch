spill_lib = static_library(
  'pg_batch_spill',
  'spill.c',
  dependencies: postgres_dep,
  include_directories: public_inc,
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: pg_pkglibdir,
  pic: true,
)

spill_dep = declare_dependency(
  link_with: spill_lib,
  dependencies: postgres_dep,
  include_directories: public_inc,
)

install_headers(
  '../include/pg_batch/spill.h',
  install_dir: pg_server_include / 'extension' / 'pg_batch',
)
