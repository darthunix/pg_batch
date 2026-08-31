project(
  'pg_batch_libraries',
  'c',
  version: '0.1.0',
  meson_version: '>=0.61.0',
  default_options: ['buildtype=release', 'c_std=c11', 'warning_level=1'],
)

pg_config = find_program(get_option('pg_config'))
pg_server_include = run_command(
  pg_config,
  '--includedir-server',
  check: true,
).stdout().strip()
pg_pkglibdir = run_command(
  pg_config,
  '--pkglibdir',
  check: true,
).stdout().strip()
pg_compile_args = []
foreach option : ['--cppflags', '--cflags', '--cflags_sl']
  pg_compile_args += run_command(
    pg_config,
    option,
    check: true,
  ).stdout().strip().split()
endforeach
pg_compile_args += '-I' + pg_server_include

postgres_dep = declare_dependency(compile_args: pg_compile_args)
public_inc = include_directories('include/pg_batch')

subdir('runtime')
subdir('kernels')
subdir('expr')
subdir('spill')

install_headers(
  'include/pg_batch/abi.h',
  'include/pg_batch/batch.h',
  'include/pg_batch/bridge.h',
  'include/pg_batch/layout.h',
  'include/pg_batch/node.h',
  'include/pg_batch/source.h',
  'include/pg_batch/arrow.h',
  install_dir: pg_server_include / 'extension' / 'pg_batch',
)

pc_data = configuration_data()
pc_data.set('PG_BATCH_VERSION', meson.project_version())
pc_data.set('PG_INCLUDEDIR_SERVER', pg_server_include)
pc_data.set('PG_PKGLIBDIR', pg_pkglibdir)

configure_file(
  input: 'pkgconfig/pg_batch-runtime.pc.in',
  output: 'pg_batch-runtime.pc',
  configuration: pc_data,
  install: true,
  install_dir: pg_pkglibdir / 'pkgconfig',
)
configure_file(
  input: 'pkgconfig/pg_batch-kernels.pc.in',
  output: 'pg_batch-kernels.pc',
  configuration: pc_data,
  install: true,
  install_dir: pg_pkglibdir / 'pkgconfig',
)
configure_file(
  input: 'pkgconfig/pg_batch-expr.pc.in',
  output: 'pg_batch-expr.pc',
  configuration: pc_data,
  install: true,
  install_dir: pg_pkglibdir / 'pkgconfig',
)
configure_file(
  input: 'pkgconfig/pg_batch-spill.pc.in',
  output: 'pg_batch-spill.pc',
  configuration: pc_data,
  install: true,
  install_dir: pg_pkglibdir / 'pkgconfig',
)
