"""
retracesoftware.utils - Runtime selectable release/debug builds

Set RETRACE_DEBUG=1 to use the debug build with symbols and assertions.
"""
import os
import weakref
import threading
from collections import UserDict
from typing import Callable, Any, Dict
from contextlib import contextmanager
from types import ModuleType


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

def typeflags(cls):
    bits = _backend_mod.type_flags(cls)
    result = set()
    for name, value in _backend_mod.TypeFlags.items():
        if (value & bits) != 0:
            result.add(name)
    return result


def is_method_descriptor(obj):
    return 'Py_TPFLAGS_METHOD_DESCRIPTOR' in typeflags(type(obj))


def flags(cls: type):
    f = _backend_mod.type_flags(cls)
    s = set()
    for name, value in _backend_mod.TypeFlags.items():
        if (f & value) != 0:
            s.add(name)
        f = f & ~value
    if f != 0:
        s.add(f)
    return s


class WithoutFlags:
    def __init__(self, cls, *flags):
        self.cls = cls
        self.flags = flags

    def __enter__(self):
        self.saved = _backend_mod.type_flags(self.cls)
        flags = self.saved
        for flag in self.flags:
            flags = flags & ~_backend_mod.TypeFlags[flag]
        _backend_mod.set_type_flags(self.cls, flags)
        return self.cls

    def __exit__(self, *args):
        _backend_mod.set_type_flags(self.cls, self.saved)


class WithFlags:
    def __init__(self, cls, *flags):
        self.cls = cls
        self.flags = flags

    def __enter__(self):
        self.saved = _backend_mod.type_flags(self.cls)
        flags = self.saved
        for flag in self.flags:
            flags |= _backend_mod.TypeFlags[flag]
        _backend_mod.set_type_flags(self.cls, flags)
        return self.cls

    def __exit__(self, *args):
        _backend_mod.set_type_flags(self.cls, self.saved)


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


def update(obj, name, f, *args, **kwargs):
    value = getattr(obj, name)
    setattr(obj, name, f(value, *args, **kwargs))


class InterceptDict(dict):
    def __init__(self, backing: Dict, on_set: Callable[[str, Any], Any]):
        super().__init__()
        self.backing = backing
        self.on_set = on_set
        self.move_from_backing()

    def move_from_backing(self):
        for key, value in self.backing.items():
            super().__setitem__(key, self.on_set(key, value))
        self.backing.clear()

    def __getitem__(self, key):
        self.move_from_backing()
        return super().__getitem__(key)
            
    def __setitem__(self, key: str, value: Any) -> None:
        self.move_from_backing()
        super().__setitem__(key, self.on_set(key, value))


def map_values(f, m):
    return {k: f(v) for k, v in m.items()}


@contextmanager
def on_set(dict, on_set):
    _backend_mod.intercept_dict_set(dict, on_set)
    yield
    _backend_mod.intercept_dict_set(dict, None)


__all__ = sorted([k for k in globals().keys() if not k.startswith("_")])
