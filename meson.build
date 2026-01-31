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
rs_extra_sources = files('cpp/generation.c')
rs_py_sources = ['src/retracesoftware/utils/__init__.py']
rs_py_install_subdir = 'retracesoftware/utils'

# Include shared build logic (builds release/debug modules)
subdir('common-headers/meson')
