# GIL Thread-Switch Monitor

Near-zero-overhead detection of GIL thread switches in CPython 3.12+, using a
preloaded mutex interposer and a separate Python C extension for activation.

## Background

### Where the GIL can switch (CPython 3.12 eval loop)

The GIL only changes hands at specific bytecode instructions where
`eval_breaker` is checked:

1. **`RESUME`** (oparg < 2) — function/module entry. Not generator resume.
2. **`JUMP_BACKWARD`** — loop back-edges. Not `JUMP_BACKWARD_NO_INTERRUPT`
   (used in `yield from`/`await`).
3. **`CALL` + specialized C-call opcodes** — after a C function returns. Not
   the "trivially fast" specializations (`CALL_NO_KW_LEN`,
   `CALL_NO_KW_ISINSTANCE`, `CALL_NO_KW_TYPE_1`, `CALL_NO_KW_LIST_APPEND`).
4. **`CALL_FUNCTION_EX`** — `*args`/`**kwargs` calls.
5. **C extensions** releasing the GIL explicitly via `Py_BEGIN_ALLOW_THREADS`.

Not yield points: `RETURN_VALUE`, `RETURN_CONST`, `YIELD_VALUE`, `SEND`,
forward jumps, `JUMP_BACKWARD_NO_INTERRUPT`.

### GIL switch frequency

Default `sys.getswitchinterval()` is 5ms. With two contending CPU-bound
threads, the maximum switch rate is ~200/sec. I/O-bound threads that release
the GIL for blocking calls can cause higher switch rates.

## Architecture

Two separate libraries with minimal coupling:

```
┌───────────────────────────────┐     ┌──────────────────────────────┐
│     libgilwatch.dylib/.so     │     │     _gilmonitor extension    │
│  (preload library)            │     │  (Python C extension)        │
│                               │     │                              │
│  Exports:                     │     │  At activation time:         │
│    gilwatch_mutex_address     │◄────│    dlsym("gilwatch_mutex_address")
│    gilwatch_callback          │◄────│    dlsym("gilwatch_callback")│
│                               │     │    finds GIL mutex from      │
│  Interposes:                  │     │    _PyRuntime                │
│    pthread_mutex_lock         │     │    sets both symbols         │
│                               │     │                              │
│  No Python dependency         │     │  Records thread switches     │
│  No version coupling          │     │  into trace stream           │
└───────────────────────────────┘     └──────────────────────────────┘
```

### Preload library (`gilwatch`)

Source: [`cpp/gilwatch.c`](cpp/gilwatch.c)

Tiny C library with no Python dependency. Interposes `pthread_mutex_lock` and
checks whether the locked mutex matches a watched address. Before activation
the address is NULL and the check is a single relaxed atomic load + branch
(~1ns per mutex call).

When the mutex matches and the acquiring thread differs from the last holder,
the callback is invoked with both `pthread_t` values:

```c
void (*gilwatch_callback)(pthread_t previous, pthread_t current);
```

Platform support:
- **macOS**: `__DATA,__interpose` section for dyld interposition (no
  constructor needed, no `dlsym(RTLD_NEXT)` at init).
- **Linux**: `dlsym(RTLD_NEXT, "pthread_mutex_lock")` resolved in a
  constructor, with a direct symbol override.

Built as part of the utils meson project — see [Building](#building) below.

### Python extension (`_gilmonitor`)

Normal C extension built with existing Python build tooling. Uses `dlsym` to
find the preload's exported symbols, then locates the GIL mutex from
`_PyRuntime` and wires everything up.

```c
// _gilmonitor.c (sketch — not yet implemented)
#include <Python.h>
#include <dlfcn.h>
#include <stdatomic.h>

static atomic_uintptr_t *watched_address = NULL;
static void (**callback_slot)(pthread_t, pthread_t) = NULL;

static void on_gil_change(pthread_t previous, pthread_t current) {
    // We hold the GIL here — safe to read Python state
    // e.g. PyEval_GetFrame() for source location
}

static PyObject* activate(PyObject *self, PyObject *args) {
    watched_address = dlsym(RTLD_DEFAULT, "gilwatch_mutex_address");
    callback_slot = dlsym(RTLD_DEFAULT, "gilwatch_callback");
    if (!watched_address || !callback_slot) {
        PyErr_SetString(PyExc_RuntimeError, "gilwatch preload not loaded");
        return NULL;
    }

    // Find GIL mutex from CPython internals
    void *runtime = dlsym(RTLD_DEFAULT, "_PyRuntime");
    // Compute offset to ceval.gil.mutex for this CPython version
    pthread_mutex_t *gil = /* offset from runtime */;

    atomic_store(watched_address, (uintptr_t)gil);
    *callback_slot = on_gil_change;
    Py_RETURN_NONE;
}
```

## Building

The gilwatch preload library is built by meson alongside the Python extension
modules. It produces `libgilwatch.dylib` (macOS) or `libgilwatch.so` (Linux):

```bash
# Normal editable install (builds everything including gilwatch)
pip install -e . --no-build-isolation

# Or standalone meson build
meson setup build
ninja -C build libgilwatch.dylib   # macOS
ninja -C build libgilwatch.so      # Linux
```

The library is installed into the Python platlib directory alongside the
extension modules, so it can be located at runtime via the package path.

## Injection point

`autoenable.py` already re-execs the Python process to enable retrace. This
is the natural place to inject the preload:

```python
# In autoenable.py, just before os.execv on line 76
import platform
preload_path = _find_gilwatch_library()
if platform.system() == 'Darwin':
    os.environ['DYLD_INSERT_LIBRARIES'] = preload_path
else:
    os.environ['LD_PRELOAD'] = preload_path

os.execv(sys.executable, new_argv)
```

The user never needs to set environment variables manually.

## Lifecycle

```
1. python app.py (with RETRACE_RECORDING or RETRACE_CONFIG set)

2. .pth triggers autoenable.py
   ├─ Sets DYLD_INSERT_LIBRARIES=gilwatch.dylib
   └─ os.execv(python, [python, -m, retracesoftware, ...])

3. Python restarts — gilwatch.dylib preloaded by dyld
   ├─ pthread_mutex_lock interposed
   └─ gilwatch_mutex_address = NULL → interposer dormant (~1ns overhead per mutex call)

4. retracesoftware.__main__ runs, proxy and stream initialized

5. _gilmonitor.activate() called
   ├─ dlsym finds gilwatch_mutex_address and gilwatch_callback
   ├─ Reads _PyRuntime to find GIL mutex address
   └─ Atomically stores address and callback → interposer active

6. Target app runs
   └─ ~200 GIL switches/sec recorded at ~0% overhead
```

## Overhead

| Source                    | Frequency        | Per-call cost | Total        |
|---------------------------|------------------|---------------|--------------|
| GIL acquire (recording)  | ~200-1000/sec    | ~50ns         | ~0.05ms/sec  |
| Non-GIL mutex lock        | ~100K-1M/sec     | ~1ns (ptr cmp)| ~0.1-1ms/sec |
| **Total**                 |                  |               | **< 0.1%**   |

## Callback data

The callback signature is `void (*)(pthread_t previous, pthread_t current)`.
It fires only when the acquiring thread differs from the last holder, so every
invocation represents an actual thread switch.

The current thread holds the GIL when the callback runs, so it can safely
access Python state:

- `PyEval_GetFrame()` — current Python frame (filename, line number)
- `_thread.get_ident()` — Python-level thread identity
- Write a `ThreadSwitchMessage` into the existing trace stream

## Platform notes

- **macOS**: `DYLD_INSERT_LIBRARIES`. SIP strips this for system binaries but
  Homebrew/pyenv Python is unaffected. Use `__DATA,__interpose` section for
  reliable dyld interposition.
- **Linux**: `LD_PRELOAD`. Standard symbol interposition via `dlsym(RTLD_NEXT, ...)`.
- **Finding the GIL mutex**: `_PyRuntime` is a public global symbol in
  libpython. The offset to `ceval.gil.mutex` is version-dependent. Alternative:
  fingerprint by observing which mutex is locked during
  `PyEval_SaveThread`/`PyEval_RestoreThread`.

## Alternatives considered

| Approach                              | Overhead   | Notes                                    |
|---------------------------------------|------------|------------------------------------------|
| `sys.monitoring` PY_START+JUMP+C_RETURN (Python) | 1.5-3x    | Fires at every potential yield point     |
| `sys.monitoring` same events (C callback)         | 1.3-2x    | Still limited by dispatch overhead       |
| `sys.settrace`                        | 3-10x      | Fires every line/call/return             |
| Frame eval hook (retrace-interpreter) | ~5-10ns/check | Good for replay, not for recording   |
| **LD_PRELOAD mutex interposer**       | **< 0.1%** | Only fires on actual GIL transitions     |
