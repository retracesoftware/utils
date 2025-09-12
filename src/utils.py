import retracesoftware_utils as _utils
from retracesoftware_utils import *
import weakref

def typeflags(cls):
    
    bits = _utils.type_flags(cls)

    result =  set()

    for name,value in _utils.TypeFlags.items():
        if (value & bits) != 0:
            result.add(name)

    return result

def is_method_descriptor(obj):
    return 'Py_TPFLAGS_METHOD_DESCRIPTOR' in typeflags(type(obj))
