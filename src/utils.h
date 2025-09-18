#pragma once
#include <Python.h>
#include <functional>
#include <signal.h>
#include <string>
#include <vector>
#include <tuple>

#include "fastcall.h"

#define MODULE "retracesoftware_utils."

#define OFFSET_OF_MEMBER(type, member) \
    ((Py_ssize_t) &reinterpret_cast<const volatile char&>((((type*)0)->member)))

void dump_stack_trace(PyThreadState * tstate);

#define CHECK_CALLABLE(name) \
    if (name) { \
        if (name == Py_None) name = nullptr; \
        else if (!PyCallable_Check(name)) { \
            PyErr_Format(PyExc_TypeError, "Parameter '%s' must be callable, but was: %S", #name, name); \
            return -1; \
        } \
    }

namespace retracesoftware {



    // extern PyTypeObject NullContext_Type;

    extern PyTypeObject Counter_Type;
    extern PyTypeObject BlockingCounter_Type;

    extern PyTypeObject Dispatch_Type;
    extern PyTypeObject ThreadState_Type;
    extern PyTypeObject ThreadStateContext_Type;
    extern PyTypeObject ThreadStateWrapped_Type;

    extern PyTypeObject Demultiplexer_Type;
    extern PyTypeObject ThreadStatePredicate_Type;
    extern PyTypeObject StripTraceback_Type;

    extern PyTypeObject StableSet_Type;
    extern PyTypeObject StableFrozenSet_Type;
    extern PyTypeObject StableSetIterator_Type;

    extern PyTypeObject FastSet_Type;
    extern PyTypeObject InstanceCheck_Type;
    extern PyTypeObject Visitor_Type;
    extern PyTypeObject Wrapped_Type;
    extern PyTypeObject Proxy_Type;
    extern PyTypeObject Reference_Type;
    extern PyTypeObject WrappedFunction_Type;
    extern PyTypeObject IdSet_Type;
    extern PyTypeObject IdSetTest_Type;
    extern PyTypeObject IdDict_Type;
    extern PyTypeObject IdSetLogical_Type;

    extern PyTypeObject ThreadSwitchMonitor_Type;
    extern PyTypeObject StripTraceback_Type;
    extern PyTypeObject Observer_Type;
    extern PyTypeObject WeakRefCallback_Type;

    void force_full_gc(void);

    PyObject * Wrapped_Target(PyObject * proxy);

    void * Reference_GetPointer(PyObject * reference);

    // PyObject * create_proxy(PyTypeObject * proxytype, PyObject * handler, PyObject * target);

    uint64_t Counter_Next(PyObject * counter);

    using Frame = std::tuple<std::string, int, std::string, std::string, std::string>;

    std::vector<Frame> stacktrace();
    PyObject * stacktrace_as_pyobject(void);

    PyObject * create_wrapped(PyTypeObject * cls, PyObject * target);
    
    // PyObject * StableSet_GetItem(PyObject * set, int index);
    // static inline vectorcallfunc extract_vectorcall(PyObject *callable)
    // {
    //     PyTypeObject *tp = Py_TYPE(callable);
    //     if (!PyType_HasFeature(tp, Py_TPFLAGS_HAVE_VECTORCALL)) {
    //         return PyObject_Vectorcall;
    //     }
    //     Py_ssize_t offset = tp->tp_vectorcall_offset;

    //     vectorcallfunc ptr;
    //     memcpy(&ptr, (char *) callable + offset, sizeof(ptr));
    //     return ptr;
    // }

    struct Wrapped : public PyObject {
        PyObject * target;
        PyObject * weakreflist;
    };
}
