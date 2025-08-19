#pragma once
#include <Python.h>
#include <functional>
#include <signal.h>
#include <string>
#include <vector>
#include <tuple>

#define MODULE "retracesoftware_utils."

#define OFFSET_OF_MEMBER(type, member) \
    ((Py_ssize_t) &reinterpret_cast<const volatile char&>((((type*)0)->member)))

void dump_stack_trace(PyThreadState * tstate);

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

    void force_full_gc(void);

    // PyObject * create_proxy(PyTypeObject * proxytype, PyObject * handler, PyObject * target);

    uint64_t Counter_Next(PyObject * counter);

    using Frame = std::tuple<std::string, int, std::string, std::string, std::string>;

    std::vector<Frame> stacktrace();
    PyObject * stacktrace_as_pyobject(void);

    // PyObject * StableSet_GetItem(PyObject * set, int index);
}
