kernels_lib = static_library(
  'pg_batch_kernels',
  'int4.c',
  'int4_hash.c',
  'int4_simd.c',
  'vector.c',
  dependencies: runtime_dep,
  include_directories: public_inc,
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: pg_pkglibdir,
  pic: true,
)

kernels_dep = declare_dependency(
  link_with: kernels_lib,
  dependencies: runtime_dep,
  include_directories: public_inc,
)

install_headers(
  '../include/pg_batch/kernels.h',
  '../include/pg_batch/vector.h',
  install_dir: pg_server_include / 'extension' / 'pg_batch',
)
