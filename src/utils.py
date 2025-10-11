import retracesoftware_utils as _utils
from retracesoftware_utils import *
import weakref
import threading

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
