"""
retracesoftware.utils - Runtime selectable release/debug builds

Set RETRACE_DEBUG=1 to use the debug build with symbols and assertions.
"""
import os
import weakref
import threading
from collections import UserDict
from typing import Any
from types import ModuleType
import retracesoftware.functional as functional
import _thread
import threading as _threading

def _is_truthy_env(v):
    if v is None:
        return False
    return v.strip().lower() in {"1", "true", "yes", "y", "on"}

_DEBUG_MODE = _is_truthy_env(os.getenv("RETRACE_DEBUG"))

_backend_mod: ModuleType
__backend__: str

try:
    if _DEBUG_MODE:
        import _retracesoftware_utils_debug as _backend_mod  # type: ignore
        __backend__ = "native-debug"
    else:
        import _retracesoftware_utils_release as _backend_mod  # type: ignore
        __backend__ = "native-release"
except Exception:
    raise ImportError("Failed to load retracesoftware_utils native extension")

# Expose debug mode flag
DEBUG_MODE = _DEBUG_MODE and __backend__.startswith("native")


def __getattr__(name: str) -> Any:
    return getattr(_backend_mod, name)


def _export_public(mod: ModuleType) -> None:
    g = globals()
    for k, v in mod.__dict__.items():
        if k.startswith("_"):
            continue
        g[k] = v


_export_public(_backend_mod)


# ---------------------------------------------------------------------------
# High-level API (convenience wrappers around C++ extension)
# ---------------------------------------------------------------------------

def wrap_func_with_overrides(func, **overrides):
    """
    Return a new function identical to `func` but with selected global names
    overridden by keyword arguments.
    """
    import builtins as _builtins
    import types

    orig = getattr(func, "__func__", func)
    g = dict(orig.__globals__)
    g.setdefault("__builtins__", _builtins.__dict__)
    g.update(overrides)

    return types.FunctionType(
        orig.__code__, g, orig.__name__, orig.__defaults__, orig.__closure__
    )


def patch_hashes(hashfunc, *types):
    """Patch ``__hash__`` on each type in *types* for deterministic ordering.

    Python's default ``__hash__`` is based on ``id()`` (memory address),
    which varies between runs.  Sets iterate in hash order, so iteration
    order becomes non-deterministic.  This replaces ``__hash__`` with
    *hashfunc*, giving stable set/dict-key ordering for record/replay.

    Call once during bootstrap, before any modules are loaded.
    """
    for cls in types:
        _backend_mod.patch_hash(cls, hashfunc)


def update(obj, name, f, *args, **kwargs):
    value = getattr(obj, name)
    setattr(obj, name, f(value, *args, **kwargs))


_thread_middleware = []

_thread.start_new_thread = _threading._start_new_thread = functional.partial(
    _backend_mod.start_new_thread_wrapper,
    _thread.start_new_thread,
    _thread_middleware)

def add_thread_middleware(factory):
    """Register a thread-spawn middleware factory.

    *factory()* is called in the parent thread (with no arguments) and
    must return a context manager (or None to skip).  The CM wraps the
    child thread's execution.

    Returns a callable that removes this middleware when called.
    """
    func = functional.repeatedly(factory)
    _thread_middleware.append(func)

    def remove():
        try:
            _thread_middleware.remove(func)
        except ValueError:
            pass

    return remove

def return_none(func):
    return functional.sequence(func, functional.constantly(None))

def chain(*funcs):
    funcs = [f for f in funcs if f is not None]
    if not funcs:
        return None

    if len(funcs) == 1:
        return funcs[0]
    else:
        funcs = [return_none(func) for func in funcs[:-1]] + [funcs[-1]]
        return functional.firstof(*funcs)

__all__ = sorted([k for k in globals().keys() if not k.startswith("_")])
