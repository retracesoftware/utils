#include "utils.h"
#include <structmember.h>

namespace retracesoftware {
    struct ThreadWatcher {
        PyObject_HEAD
        PyObject * on_thread_switch;
        PyThreadState * last_thread_state;

        vectorcallfunc vectorcall;
        PyObject * target;
        vectorcallfunc target_func;
    };

    static bool before(ThreadWatcher * self) {
        // assert(PyCallable_Check(self->on_thread_switch));

        if (PyThreadState_Get() != self->last_thread_state) {
            self->last_thread_state = PyThreadState_Get();
            PyObject * res = PyObject_CallNoArgs(self->on_thread_switch);
            
            // printf("FOO!!!!! %p\n", res);

            Py_XDECREF(res);

            if (!res) return false;
        }
        return true;
    }

    static PyObject * vectorcall(ThreadWatcher * self, PyObject* const* args, size_t nargsf, PyObject* kwnames) {
        if (!before(self)) return nullptr;

        return self->target_func(self->target, args, nargsf, kwnames);
    }

    static PyObject * getattro(ThreadWatcher *self, PyObject *name) {
        if (!before(self)) return nullptr;

        return PyObject_GetAttr(self->target, name);
    }

    static int setattro(ThreadWatcher *self, PyObject * name, PyObject *value) {
        if (!before(self)) return -1;

        return PyObject_SetAttr(self->target, name, value);
    }

    static int traverse(ThreadWatcher* self, visitproc visit, void* arg) {
        Py_VISIT(self->on_thread_switch);
        Py_VISIT(self->target);
        return 0;
    }

    static int clear(ThreadWatcher* self) {
        Py_CLEAR(self->on_thread_switch);
        Py_CLEAR(self->target);
        return 0;
    }

    static void dealloc(ThreadWatcher *self) {
        PyObject_GC_UnTrack(self);          // Untrack from the GC
        clear(self);
        Py_TYPE(self)->tp_free((PyObject *)self);  // Free the object
    }

    static PyMemberDef members[] = {
        // {"elements", T_OBJECT, offsetof(First, elements), READONLY, "TODO"},
        {NULL}  /* Sentinel */
    };

    static PyObject * str(ThreadWatcher *self) {
        return PyObject_Str(self->target);
    }

    static PyObject * repr(ThreadWatcher *self) {
        return PyObject_Repr(self->target);
    }

    static int init(ThreadWatcher *self, PyObject *args, PyObject *kwds) {

        PyObject * function = NULL;
        PyObject * on_thread_switch = NULL;

        static const char *kwlist[] = {
            "target",
            "on_thread_switch",
            NULL};

        if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO", (char **)kwlist, &function, &on_thread_switch))
        {
            return -1; // Return NULL on failure
        }

        CHECK_CALLABLE(function);
        CHECK_CALLABLE(on_thread_switch);
        
        self->target = Py_XNewRef(function);
        self->on_thread_switch = Py_XNewRef(on_thread_switch);
        self->vectorcall = (vectorcallfunc)vectorcall;
        self->last_thread_state = PyThreadState_Get();
        self->target_func = extract_vectorcall(function);
        return 0;
    }

    PyTypeObject ThreadWatcher_Type = {
        .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = MODULE "threadwatcher",
        .tp_basicsize = sizeof(ThreadWatcher),
        .tp_itemsize = 0,
        .tp_dealloc = (destructor)dealloc,
        .tp_vectorcall_offset = offsetof(ThreadWatcher, vectorcall),
        .tp_repr = (reprfunc)repr,
        .tp_call = PyVectorcall_Call,
        .tp_str = (reprfunc)str,
        .tp_getattro = (getattrofunc)getattro,
        .tp_setattro = (setattrofunc)setattro,
        .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_HAVE_VECTORCALL,
        .tp_doc = "TODO",
        .tp_traverse = (traverseproc)traverse,
        .tp_clear = (inquiry)clear,
        // .tp_methods = methods,
        .tp_members = members,
        .tp_init = (initproc)init,
        .tp_new = PyType_GenericNew,
    };
}