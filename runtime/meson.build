runtime_lib = static_library(
  'pg_batch_runtime',
  'api.c',
  'adapter.c',
  'builder.c',
  'input.c',
  'output.c',
  'unary.c',
  'plan.c',
  'planner.c',
  dependencies: postgres_dep,
  include_directories: public_inc,
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: pg_pkglibdir,
  pic: true,
)

runtime_dep = declare_dependency(
  link_with: runtime_lib,
  dependencies: postgres_dep,
  include_directories: public_inc,
)

install_headers(
  '../include/pg_batch/runtime.h',
  '../include/pg_batch/plan.h',
  '../include/pg_batch/planner.h',
  install_dir: pg_server_include / 'extension' / 'pg_batch',
)
