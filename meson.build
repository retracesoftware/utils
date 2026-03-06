# meson.build - Project configuration + shared build logic

project(
  '_retracesoftware_utils',
  ['cpp', 'c'],
  version: run_command('python', '-m', 'setuptools_scm', check: true, capture: true).stdout().strip(),
  default_options: ['cpp_std=c++20', 'strip=false'])

# Project-specific configuration
rs_extension_name = '_retracesoftware_utils'
rs_cpp_source_dir = 'cpp'
rs_ext_install_subdir = ''
rs_extra_sources = files('cpp/generation.c', 'cpp/gilwatch_activate.c')
rs_py_sources = [
  'src/retracesoftware/utils/__init__.py',
  'src/retracesoftware/utils/breakpoint.py',
  'src/retracesoftware/utils/threadcontext.py',
  'src/retracesoftware/utils/trace.py',
]
rs_py_install_subdir = 'retracesoftware/utils'

# Include shared build logic (builds release/debug modules)
subdir('common-headers/meson')

# gilwatch — preload library for detecting GIL thread switches.
# Plain C, no Python dependency. Loaded via LD_PRELOAD / DYLD_INSERT_LIBRARIES.
_gilwatch_deps = [dependency('threads')]
if host_machine.system() != 'darwin'
  _gilwatch_deps += [meson.get_compiler('c').find_library('dl')]
endif

shared_library(
  'gilwatch',
  files('cpp/gilwatch.c'),
  dependencies: _gilwatch_deps,
  install: true,
  install_dir: py.get_install_dir(),
  c_args: ['-fvisibility=default'],
)
