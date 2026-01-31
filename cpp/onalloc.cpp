#include "utils.h"
#include <exception>
#include <structmember.h>
#include "unordered_dense.h"
using namespace ankerl::unordered_dense;

namespace retracesoftware {

    static map<PyTypeObject *, allocfunc> allocfuncs;

    static PyObject * key() {
        static PyObject * name = nullptr;
        if (!name) name = PyUnicode_InternFromString("__retrace_on_alloc__");
        return name;
    }

    static PyObject * find_callback(PyTypeObject * cls) {
        while (cls) {
            PyObject * callback = PyDict_GetItem(cls->tp_dict, key());

            if (callback) return callback;
        }
        return nullptr;
    }
    
    static bool call_callback(PyObject * allocated) {
        PyObject * callback = find_callback(Py_TYPE(allocated));

        if (callback) {
            PyObject * result = PyObject_CallOneArg(callback, allocated);
            Py_XDECREF(result);
            if (!result) {
                return false;
            }
        }
        return true;
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
    
    static bool is_patched(allocfunc func) {
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

    bool set_on_alloc(PyTypeObject *type, PyObject * callback) {
        if (!is_patched(type->tp_alloc)) {
            patch_alloc(type);
        }
        return PyDict_SetItem(type->tp_dict, key(), callback) == 0 ? true : false;
    }
}