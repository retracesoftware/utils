#include "utils.h"
#include <structmember.h>

#if defined(__GNUC__) || defined(__clang__)
#include <alloca.h>
#elif defined(_MSC_VER)
#include <malloc.h>
#define alloca _alloca
#endif

namespace retracesoftware {

    struct NewWrapper : public PyObject {
        newfunc target;
        PyObject * handler;
        vectorcallfunc vectorcall;

        PyObject * call(PyTypeObject * cls, PyObject * args, PyObject * kwargs) {

            size_t nargs = PyTuple_GET_SIZE(args);
            size_t nargsf = (nargs + 1) | PY_VECTORCALL_ARGUMENTS_OFFSET;

            if (kwargs && PyDict_Size(kwargs) > 0) {
                PyObject *keys_list = PyDict_Keys(kwargs);
                PyObject * keys = PySequence_Tuple(keys_list);
                Py_DECREF(keys_list);

                size_t total_args = nargs + PyTuple_GET_SIZE(keys);

                PyObject ** args = (PyObject **)alloca((total_args + 2) * sizeof(PyObject *)) + 1;

                args[0] = this;
                PyObject ** current = args + 1;

                for (size_t i = 0; i < nargs; i++) {
                    *current++ = PyTuple_GET_ITEM(args, i);
                }
                for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(keys); i++) {
                    PyObject * key = PyTuple_GET_ITEM(keys, i);
                    PyObject * value = PyDict_GetItem(kwargs, key);
                    *current++ = value;
                }

                PyObject * result = PyObject_Vectorcall(handler, args, nargsf, keys);
                Py_DECREF(keys);
                return result;

            } else {
                PyObject ** args = (PyObject **)alloca((nargs + 2) * sizeof(PyObject *)) + 1;
                
                args[0] = this;

                for (size_t i = 0; i < nargs; i++) {
                    args[i + 1] = PyTuple_GET_ITEM(args, i);
                }
                return PyObject_Vectorcall(handler, args, nargsf, nullptr);
            }
        }

        static PyObject * create_posargs(size_t n, PyObject* const * args) {
            PyObject * tuple = PyTuple_New(n);
            for (size_t i = 0; i < n; i++) {
                PyTuple_SET_ITEM(tuple, i, Py_NewRef(args[i]));
            }
            return tuple;
        }

        static PyObject * create_kwargs(PyObject* kwnames, PyObject* const * args) {
            PyObject * dict = PyDict_New();

            for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwnames); i++) {
                PyDict_SetItem(dict, PyTuple_GET_ITEM(kwnames, i), args[i]);
            }
            return dict;
        }

        static PyObject * py_vectorcall(NewWrapper * self, PyObject* const * args, size_t nargsf, PyObject* kwnames) {
            size_t nargs = PyVectorcall_NARGS(nargsf);

            PyObject * posargs = create_posargs(nargs, args);
            PyObject * kwargs = kwnames ? create_kwargs(kwnames, args + nargs) : nullptr;

            PyObject * result = self->target((PyTypeObject *)args[0], posargs, kwargs);
            Py_DECREF(posargs);
            Py_XDECREF(kwargs);
            return result;
        }

        static int traverse(NewWrapper* self, visitproc visit, void* arg) {
            Py_VISIT(self->handler);
            return 0;
        }

        static int clear(NewWrapper* self) {
            Py_CLEAR(self->handler);
            return 0;
        }

        static void dealloc(NewWrapper *self) {
            PyObject_GC_UnTrack(self);          // Untrack from the GC
            clear(self);
            Py_TYPE(self)->tp_free((PyObject *)self);  // Free the object
        }

    };
    
    static PyObject * retrace_key = nullptr;

    static PyObject * new_wrapper(PyTypeObject * cls, PyObject * args, PyObject * kwargs) {
        // step 1, find the NewWrapper object in the cls dict

        NewWrapper * wrapper = (NewWrapper *)PyDict_GetItem(cls->tp_dict, retrace_key);

        return wrapper->call(cls, args, kwargs);
    }

    bool install_new_wrapper(PyTypeObject * cls, PyObject * handler) {
        if (PyType_HasFeature(cls, Py_TPFLAGS_BASETYPE)) {
            PyErr_Format(PyExc_ValueError, "Cannot install handler to class: %S as it is a base type", cls);
            return false;
        }

        if (!retrace_key) retrace_key = PyUnicode_InternFromString("__retrace_new__");
    
        NewWrapper * wrapper = reinterpret_cast<NewWrapper *>(NewWrapper_Type.tp_alloc(&NewWrapper_Type, 0));

        wrapper->vectorcall = (vectorcallfunc)NewWrapper::py_vectorcall;
        wrapper->target = cls->tp_new;
        wrapper->handler = Py_NewRef(handler);

        PyDict_SetItem(cls->tp_dict, retrace_key, wrapper);
        Py_DECREF(wrapper);

        cls->tp_new = new_wrapper;
        return true;
    }

    PyTypeObject NewWrapper_Type = {
        .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = MODULE "newwrapper",
        .tp_basicsize = sizeof(NewWrapper),
        .tp_itemsize = 0,
        .tp_dealloc = (destructor)NewWrapper::dealloc,
        .tp_vectorcall_offset = OFFSET_OF_MEMBER(NewWrapper, vectorcall),
        .tp_call = PyVectorcall_Call,
        .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_HAVE_VECTORCALL,
        .tp_doc = "TODO",
        .tp_traverse = (traverseproc)NewWrapper::traverse,
        .tp_clear = (inquiry)NewWrapper::clear,
        // .tp_methods = methods,
        // .tp_members = members,
        // .tp_descr_get = tp_descr_get,
        // .tp_init = (initproc)init,
        // .tp_new = PyType_GenericNew,
    };
    // we pop the newwrapper into class under a known key
}