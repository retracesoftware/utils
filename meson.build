# meson.build
project('retracesoftware_utils', ['cpp', 'c'], default_options: ['cpp_std=c++20'])

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
   'src/wrappedmember.cpp',
   'src/reference.cpp',
   'src/threadswitchmonitor.cpp',
   'src/proxy.cpp',
   'src/idset.cpp',
   'src/iddict.cpp',
   'src/perthread.cpp',
   'src/patchhash.cpp',
   'src/observer.cpp',
   'src/frameeval.cpp',
   'src/newwrapper.cpp',
   'src/marker.cpp',
   'src/onalloc.cpp',
   'src/dictintercept.cpp',
   'src/generation.c',
   'src/gccollectpred.cpp',
   'src/runall.cpp',
   ],
  install: true,

  # If you want extra flags:
  # cpp_args: ['-O3', '-fvisibility=hidden'],
  include_directories: include_directories('common-headers/include'),
)