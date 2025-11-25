import retracesoftware_utils as _utils
from retracesoftware_utils import *
import weakref
import threading
from collections import UserDict
from typing import Callable, Any
from contextlib import contextmanager

def typeflags(cls):
    
    bits = _utils.type_flags(cls)

    result =  set()

    for name,value in _utils.TypeFlags.items():
        if (value & bits) != 0:
            result.add(name)

    return result

def is_method_descriptor(obj):
    return 'Py_TPFLAGS_METHOD_DESCRIPTOR' in typeflags(type(obj))

def flags(cls : type):
    f = _utils.type_flags(cls)

    s = set()

    for name,value in _utils.TypeFlags.items():
        if (f & value) != 0:
            s.add(name)
        f = f & ~value

    if f != 0:
        s.add(f)

    return s

class WithoutFlags:

    def __init__(self, cls  , *flags):
        self.cls = cls
        self.flags = flags

    def __enter__(self):
        self.saved = utils.type_flags(self.cls)
        flags = self.saved

        for flag in self.flags:
            flags = flags & ~_utils.TypeFlags[flag]

        _utils.set_type_flags(self.cls, flags)
        return self.cls

    def __exit__(self, *args):
        _utils.set_type_flags(self.cls, self.saved)

class WithFlags:

    def __init__(self, cls  , *flags):
        self.cls = cls
        self.flags = flags

    def __enter__(self):
        self.saved = _utils.type_flags(self.cls)
        flags = self.saved

        for flag in self.flags:
            flags |= _utils.TypeFlags[flag]

        _utils.set_type_flags(self.cls, flags)
        return self.cls

    def __exit__(self, *args):
        _utils.set_type_flags(self.cls, self.saved)

def wrap_func_with_overrides(func, **overrides):
    """
    Return a new function identical to `func` but with selected global names
    overridden by keyword arguments.

    Example:
        new_func = wrap_func_with_overrides(old_func, exec=my_exec, print=my_print)
    """

    import builtins as _builtins
    import types

    # Unwrap bound method if needed
    orig = getattr(func, "__func__", func)

    # Clone globals so we don't mutate the original module dict
    g = dict(orig.__globals__)
    g.setdefault("__builtins__", _builtins.__dict__)
    g.update(overrides)

    # Recreate the function
    return types.FunctionType(
        orig.__code__, g, orig.__name__, orig.__defaults__, orig.__closure__
    )

def update(obj, name, f, *args, **kwargs):
    value = getattr(obj, name)
    setattr(obj, name, f(value, *args, **kwargs))

from typing import Dict, Any

class InterceptDict(dict):
    def __init__(self, backing: Dict, on_set: Callable[[str, Any], Any]):
        """
        A dictionary-like class that wraps an existing dictionary and calls a callback
        to potentially modify values before they are added or updated.
        
        Args:
            underlying_dict: The existing dictionary to wrap and modify directly.
            on_set: Function that takes (key, value) and returns the value to store.
            *args: Iterable of key-value pairs to update the underlying dict.
            **kwargs: Keyword arguments to update the underlying dict.
        """
        super().__init__()
        self.backing = backing
        self.on_set = on_set
        self.move_from_backing()
        # self.fallback = initial

        # store the dict version on the backing object

        # Process additional items through the callback
        # updates = dict(*args, **kwargs)
        # for key, value in updates.items():
        #     modified_value = self.on_set(key, value)
        #     self._dict[key] = modified_value

    def move_from_backing(self):
        for key,value in self.backing.items():
            super().__setitem__(key, self.on_set(key, value))
        self.backing.clear()

    def __getitem__(self, key):
        self.move_from_backing()
        return super().__getitem__(key)
            
    def __setitem__(self, key: str, value: Any) -> None:
        """Set an item in the underlying dict, modifying the value via callback."""
        self.move_from_backing()
        super().__setitem__(key, self.on_set(key, value))
    

    # def update(self, *args, **kwargs) -> None:
    #     """Update the underlying dict, processing values through the callback."""
    #     updates = dict(*args, **kwargs)
    #     for key, value in updates.items():            
    #         self._dict[key] = self.on_set(key, value)
    
def map_values(f, m):
    return {k: f(v) for k,v in m.items()}

@contextmanager
def on_set(dict, on_set):
    _utils.intercept_dict_set(dict, on_set)
    yield
    _utils.intercept_dict_set(dict, None)
