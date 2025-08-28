#include "utils.h"
#include <structmember.h>

#if defined(__GNUC__) || defined(__clang__)
#include <alloca.h>
#elif defined(_MSC_VER)
#include <malloc.h>
#define alloca _alloca
#endif

namespace retracesoftware {

    struct WrappedFunction : public Wrapped {
        PyObject * handler;
        vectorcallfunc handler_vectorcall;
        vectorcallfunc vectorcall;

        static int traverse(WrappedFunction* self, visitproc visit, void* arg) {
            Py_VISIT(self->handler);
            return Wrapped_Type.tp_traverse(self, visit, arg);
        }
    
        static int clear(WrappedFunction * self) {
            Py_CLEAR(self->handler);
            return Wrapped_Type.tp_clear(self);
        }

        PyObject * call_with_alloca(PyObject* const * args, size_t nargs, PyObject* kwnames) {
            size_t total_args = nargs + (kwnames ? PyTuple_GET_SIZE(nargs) : 0) + 1;

            PyObject ** mem = (PyObject **)alloca(sizeof(PyObject *) * (total_args + 1)) + 1;

            mem[0] = this;
            for (size_t i = 0; i < nargs; i++) {
                mem[i + 1] = args[i];
            }
            size_t nargsf = (nargs + 1) | PY_VECTORCALL_ARGUMENTS_OFFSET;

            return handler_vectorcall(handler, mem, nargsf, kwnames);
        }

        PyObject * call(PyObject* const * args, size_t nargsf, PyObject* kwnames) {

            if (nargsf & PY_VECTORCALL_ARGUMENTS_OFFSET) {

                PyObject * saved = args[-1];
                ((PyObject **)args)[-1] = this;

                PyObject * result = handler_vectorcall(handler, args - 1, PyVectorcall_NARGS(nargsf) + 1, kwnames);
                ((PyObject **)args)[-1] = saved;
                return result;
            } else {
                return call_with_alloca(args, nargsf, kwnames);
            }
        }

        static PyObject * py_vectorcall(PyObject * self, PyObject* const * args, size_t nargsf, PyObject* kwnames) {
            return reinterpret_cast<WrappedFunction *>(self)->call(args, nargsf, kwnames);
        }

        static int init(WrappedFunction * self, PyObject * args, PyObject * kwargs) {
            PyObject * handler;
            PyObject * target;
            static const char *kwlist[] = {"target", "handler", NULL};  // List of keyword

            if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO", (char **)kwlist, &target, &handler)) {
                return -1;
            }
            self->handler = Py_NewRef(handler);
            self->target = Py_NewRef(target);
            self->handler_vectorcall = extract_vectorcall(handler);
            self->vectorcall = py_vectorcall;

            return 0;
        }

        static PyObject* tp_descr_get(PyObject *self, PyObject *obj, PyObject *type) {
            return obj == NULL || obj == Py_None ? Py_NewRef(self) : PyMethod_New(self, obj);
        }

        static PyObject * repr(WrappedFunction *self) {
            return PyUnicode_FromFormat("<wrapped_function %S>", self->target);
        }
    };

    // for method descriptor, extend partial

    // static PyObject * MethodDescriptor_repr(MethodDescriptor *self) {
    //     return PyUnicode_FromFormat("Proxied<%S>", self->name);
    // }

    // static PyMemberDef MethodDescriptor_members[] = {
    //     {"__objclass__", T_OBJECT, OFFSET_OF_MEMBER(MethodDescriptor, objclass), READONLY, "TODO"},
    //     {"__name__", T_OBJECT, OFFSET_OF_MEMBER(MethodDescriptor, name), READONLY, "TODO"},
    //     {NULL}  /* Sentinel */
    // };

    // static PyObject* MethodDescriptor_getattro(MethodDescriptor *self, PyObject * name) {

    //     PyObject * first_try = PyObject_GenericGetAttr((PyObject *)self, name);
    
    //     if (first_try) return first_try;
    
    //     PyErr_Clear();
    
    //     return Py_TYPE(self->target)->tp_getattro(self->target, name);
    // }    

    PyTypeObject WrappedFunction_Type = {
        .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = MODULE "wrapped_function",
        .tp_basicsize = sizeof(WrappedFunction),
        .tp_itemsize = 0,
        // .tp_dealloc = (destructor)MethodDescriptor_dealloc,
        .tp_vectorcall_offset = OFFSET_OF_MEMBER(WrappedFunction, vectorcall),
        .tp_repr = (reprfunc)WrappedFunction::repr,
        .tp_call = PyVectorcall_Call,
        // .tp_getattro = (binaryfunc)MethodDescriptor_getattro,
        .tp_str = (reprfunc)WrappedFunction::repr,

        .tp_flags = Py_TPFLAGS_DEFAULT |
                    Py_TPFLAGS_HAVE_GC |
                    Py_TPFLAGS_HAVE_VECTORCALL |
                    Py_TPFLAGS_METHOD_DESCRIPTOR,
        .tp_doc = "TODO",
        .tp_traverse = (traverseproc)WrappedFunction::traverse,
        .tp_clear = (inquiry)WrappedFunction::clear,
        // .tp_members = MethodDescriptor_members,
        .tp_base = &Proxy_Type,
        .tp_descr_get = WrappedFunction::tp_descr_get,
        .tp_init = (initproc)WrappedFunction::init,
        // .tp_new = Wrapped_Type.tp_new,
    };
}