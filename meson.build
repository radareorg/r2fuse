project('r2fuse', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-only',
  meson_version: '>=0.60.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'])

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments([
  '-Wno-unused-parameter', '-Wno-sign-compare',
  '/wd4100', '/wd4244', '/wd4267', '/wd4018',
]), language: 'c')

static = get_option('static')
r2_deps = []
r_fs_dep = dependency('r_fs', static: static, required: host_machine.system() != 'windows')
if r_fs_dep.found()
  r2_deps += [r_fs_dep, dependency('r_io', static: static)]
else
  # windows without pkg-config: the release zip ships headers in include/libr
  # and import libs in lib
  r2_prefix = get_option('r2_prefix')
  if r2_prefix == ''
    r2_prefix = run_command(find_program('radare2'), '-H', 'R2_PREFIX', check: true).stdout().strip()
  endif
  r2_inc = include_directories(r2_prefix / 'include' / 'libr', r2_prefix / 'include' / 'libr' / 'sdb')
  r2_libdir = r2_prefix / 'lib'
  foreach l: ['r_fs', 'r_io', 'r_util', 'r_cons', 'r_socket', 'r_muta']
    lib = cc.find_library(l, dirs: r2_libdir, required: false)
    if not lib.found()
      lib = cc.find_library('lib' + l, dirs: r2_libdir, required: l in ['r_fs', 'r_io', 'r_util'])
    endif
    r2_deps += lib
  endforeach
  r2_deps += declare_dependency(include_directories: r2_inc)
endif

if host_machine.system() == 'windows'
  # winfsp provides the fuse3 compatible api, its dll is always loaded at runtime
  winfsp = get_option('winfsp_prefix')
  winfsp_arch = {'x86_64': 'x64', 'aarch64': 'a64'}.get(host_machine.cpu_family(), 'x86')
  fuse_dep = declare_dependency(
    include_directories: include_directories(winfsp / 'inc' / 'fuse3', winfsp / 'inc'),
    dependencies: cc.find_library('winfsp-' + winfsp_arch, dirs: winfsp / 'lib'),
    link_args: ['/DELAYLOAD:winfsp-' + winfsp_arch + '.dll', 'delayimp.lib'])
else
  fuse_dep = dependency('fuse3', static: static)
endif

link_args = []
if static and host_machine.system() == 'linux'
  link_args += ['-static']
endif

executable('r2fuse',
  files('src/main.c', 'src/rfs.c', 'src/check.c'),
  dependencies: r2_deps + [fuse_dep],
  link_args: link_args,
  install: true)

test('harness', find_program('sh'), args: [meson.current_source_dir() / 'test/harness.sh'])

test('check', find_program('test/run.sh'),
  args: [get_option('testbins')],
  depends: [],
  env: ['R2FUSE=' + meson.current_build_dir() / 'r2fuse'],
  timeout: 300)
