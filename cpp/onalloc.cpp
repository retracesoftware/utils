#include "utils.h"
#include <exception>
#include <structmember.h>
#include "unordered_dense.h"
using namespace ankerl::unordered_dense;

namespace retracesoftware {

    static map<PyTypeObject *, allocfunc> allocfuncs;
    static map<PyTypeObject *, destructor> deallocfuncs;
    static map<PyObject *, PyObject *> dealloc_callbacks;

    static PyObject * key() {
        static PyObject * name = nullptr;
        if (!name) name = PyUnicode_InternFromString("__retrace_on_alloc__");
        return name;
    }

    static PyObject * find_callback(PyTypeObject * cls) {
        while (cls) {
            PyObject * callback = PyDict_GetItem(cls->tp_dict, key());
            if (callback) return callback;
            cls = cls->tp_base;
        }
        return nullptr;
    }

    static bool call_callback(PyObject * allocated) {
        PyObject * callback = find_callback(Py_TYPE(allocated));
        if (!callback) return true;

        PyObject * result = PyObject_CallOneArg(callback, allocated);
        if (!result) return false;

        if (result == Py_None) {
            Py_DECREF(result);
            return true;
        }

        if (PyCallable_Check(result)) {
            dealloc_callbacks[allocated] = result;  // steals the reference
            return true;
        }

        Py_DECREF(result);
        PyErr_Format(PyExc_TypeError,
            "__retrace_on_alloc__ callback must return None or a callable, "
            "got %.200s", Py_TYPE(result)->tp_name);
        return false;
    }

    // generic_dealloc is only installed on C base types (never heap types).
    // CPython's subtype_dealloc on any heap subtype naturally walks tp_base
    // until it finds this trampoline and calls it — no re-entry possible.
    static void generic_dealloc(PyObject * obj) {
        auto it = dealloc_callbacks.find(obj);
        if (it != dealloc_callbacks.end()) {
            PyObject * cb = it->second;
            dealloc_callbacks.erase(it);

            Py_SET_REFCNT(obj, 1);
            PyObject * r = PyObject_CallNoArgs(cb);
            Py_XDECREF(r);
            Py_DECREF(cb);
            if (!r) PyErr_Clear();

            if (Py_REFCNT(obj) > 1) {
                Py_SET_REFCNT(obj, Py_REFCNT(obj) - 1);
                return;
            }
            Py_SET_REFCNT(obj, 0);
        }

        PyTypeObject * type = Py_TYPE(obj);
        while (type && type->tp_dealloc != generic_dealloc)
            type = type->tp_base;
        if (!type) return;

        auto dit = deallocfuncs.find(type);
        if (dit != deallocfuncs.end())
            dit->second(obj);
    }

    static PyObject * generic_alloc(PyTypeObject *type, Py_ssize_t nitems) {

        auto it = allocfuncs.find(type);
        if (it == allocfuncs.end()) {
            PyErr_Format(PyExc_RuntimeError, "Original tp_alloc mapping for type: %S not found",
                            type);
            return nullptr;
        }
        PyObject * obj = it->second(type, nitems);

        if (obj) {
            if (!call_callback(obj)) {
                Py_DECREF(obj);
                return nullptr;
            }
        }
        return obj;
    }

    static PyObject * PyType_GenericAlloc_Wrapper(PyTypeObject *type, Py_ssize_t nitems) {

        PyObject * obj = PyType_GenericAlloc(type, nitems);

        if (obj) {
            if (!call_callback(obj)) {
                Py_DECREF(obj);
                return nullptr;
            }
        }
        return obj;
    }

    static bool is_alloc_patched(allocfunc func) {
        return func == PyType_GenericAlloc_Wrapper || func == generic_alloc;
    }

    static void patch_alloc(PyTypeObject * cls) {
        if (cls->tp_alloc == PyType_GenericAlloc) {
            cls->tp_alloc = PyType_GenericAlloc_Wrapper;
        } else {
            allocfuncs[cls] = cls->tp_alloc;
            cls->tp_alloc = generic_alloc;
        }
    }

    static void patch_dealloc(PyTypeObject * cls) {
        if (cls->tp_dealloc && cls->tp_dealloc != generic_dealloc) {
            deallocfuncs[cls] = cls->tp_dealloc;
            cls->tp_dealloc = generic_dealloc;
        }
    }

    static void patch_alloc_subclasses(PyTypeObject * type) {
        PyObject * subs = PyObject_CallMethod((PyObject *)type, "__subclasses__", NULL);
        if (!subs) { PyErr_Clear(); return; }

        Py_ssize_t n = PyList_Size(subs);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyTypeObject * sub = (PyTypeObject *)PyList_GetItem(subs, i);
            if (!is_alloc_patched(sub->tp_alloc))
                patch_alloc(sub);
            patch_alloc_subclasses(sub);
        }
        Py_DECREF(subs);
    }

    bool set_on_alloc(PyTypeObject *type, PyObject * callback) {
        if (type->tp_flags & Py_TPFLAGS_HEAPTYPE) {
            PyErr_Format(PyExc_TypeError,
                "set_on_alloc requires a C base type, got heap type %.200s",
                type->tp_name);
            return false;
        }

        if (!is_alloc_patched(type->tp_alloc))
            patch_alloc(type);
        patch_alloc_subclasses(type);

        patch_dealloc(type);

        return PyDict_SetItem(type->tp_dict, key(), callback) == 0;
    }
}