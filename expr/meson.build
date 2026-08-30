expr_lib = static_library(
  'pg_batch_expr',
  'expr.c',
  'projection.c',
  dependencies: kernels_dep,
  include_directories: expr_inc,
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: pg_pkglibdir,
  pic: true,
)

expr_dep = declare_dependency(
  link_with: expr_lib,
  dependencies: kernels_dep,
  include_directories: expr_inc,
)

install_headers(
  'include/expr.h',
  install_dir: pg_server_include / 'extension' / 'pg_batch_expr',
)
