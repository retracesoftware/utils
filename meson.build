# meson.build
project('retracesoftware_utils', 'cpp', default_options: ['cpp_std=c++20'])

# Bind to the interpreter Meson/meson-python wants us to use.
py = import('python').find_installation()

py.install_sources(
  'src/utils.py',
  subdir: 'retracesoftware')

py.extension_module(
  'retracesoftware_utils',
  ['src/module.cpp',
   'src/stableset.cpp',
   'src/counter.cpp',
   'src/demux.cpp',
   'src/threadstate.cpp',
   'src/striptraceback.cpp',
   'src/stacktrace.cpp',
   'src/fastset.cpp',
   'src/instancecheck.cpp',
   'src/visitor.cpp',
   'src/wrapped.cpp',
   'src/wrappedfunction.cpp',
   'src/reference.cpp',
   'src/threadswitchmonitor.cpp',
   'src/proxy.cpp',
   'src/idset.cpp',
   'src/iddict.cpp',
   'src/striptraceback.cpp',
   'src/observer.cpp',
   'src/patchhash.cpp',
   'src/perthread.cpp'
   ],
  install: true,

  # If you want extra flags:
  # cpp_args: ['-O3', '-fvisibility=hidden'],
  include_directories: include_directories('common-headers/include'),
)