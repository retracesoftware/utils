#include "utils.h"
#include <signal.h>

static PyObject * set_type(PyObject * self, PyObject * args, PyObject *kwds) {

    PyTypeObject * new_type;
    PyObject * obj;

    static const char *kwlist[] = {"obj", "type", NULL};  // List of keyword

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO!", (char **)kwlist, &obj, &PyType_Type, &new_type)) {
        return NULL;
    }

    if (!PyType_IsSubtype(new_type, Py_TYPE(obj))) {
        PyErr_Format(PyExc_TypeError, "Cannot set type of object: %S to type: %S as not a subtype of object type: %S",
            obj, (PyObject *)new_type, Py_TYPE(obj));
        return nullptr;
    }

    if (new_type->tp_basicsize != Py_TYPE(obj)->tp_basicsize) {
        PyErr_Format(PyExc_TypeError, "Cannot set type of object: %S to type: %S as memory layout differs",
            obj, (PyObject *)new_type);
        return nullptr;
    }

    Py_DECREF(Py_TYPE(obj));
    Py_INCREF(new_type);
    Py_SET_TYPE(obj, new_type);

    Py_RETURN_NONE;
}

static PyObject * noop(PyObject *module, PyObject * const * args, size_t nargs, PyObject* kwnames) {
    Py_RETURN_NONE;
}

static PyObject * type_flags(PyObject * module, PyObject * arg) {
    if (!PyType_Check(arg)) {
        PyErr_Format(PyExc_TypeError, "Passed object: %S is not a type", arg);
        return NULL;
    }
    PyTypeObject * cls = (PyTypeObject *)arg;

    return PyLong_FromUnsignedLong(cls->tp_flags);
}

static PyObject * set_type_flags(PyObject * module, PyObject * args) {
    PyTypeObject * cls;
    unsigned long flags;

    if (!PyArg_ParseTuple(args, "O!k", &PyType_Type, &cls, &flags)) {
        return NULL;  // Error in parsing
    }

    PyType_Modified(cls);
    cls->tp_flags = flags;
    PyType_Modified(cls);

    Py_RETURN_NONE;
}

void dump_stack_trace(PyThreadState *tstate) {
    // PyThreadState *tstate = PyThreadState_Get();
    if (!tstate) {
        fprintf(stderr, "No thread state!\n");
        return;
    }

    PyFrameObject *frame = PyThreadState_GetFrame(tstate);
    while (frame) {
        PyCodeObject *code = PyFrame_GetCode(frame);  // New reference
        int lineno = PyFrame_GetLineNumber(frame);

        const char *filename = PyUnicode_AsUTF8(code->co_filename);
        const char *funcname = PyUnicode_AsUTF8(code->co_name);

        if (filename && funcname) {
            fprintf(stderr, "File \"%s\", line %d, in %s\n", filename, lineno, funcname);
        } else {
            fprintf(stderr, "<could not decode frame>\n");
        }
        
        Py_DECREF(code);
        frame = PyFrame_GetBack(frame);
        // frame->f_back;
    }
}

static PyObject * sigtrap(PyObject * module, PyObject * obj) {
    PyObject *s = PyObject_Str(obj);
    printf("%s\n", PyUnicode_AsUTF8(s));
    Py_DECREF(s);

    dump_stack_trace(PyThreadState_Get());
    raise(SIGTRAP);
    Py_RETURN_NONE;
}

static PyObject * raise_exception(PyObject * module, PyObject * const * args, size_t nargs) {
    if (nargs != 2) {
        PyErr_SetString(PyExc_TypeError, "raise_exception takes two arguments, type and exception");
        return nullptr;
    }
    PyErr_SetObject(args[0], Py_NewRef(args[1]));
    // PyErr_Restore(args[0], args[1], nullptr);
    // PyErr_Restore(Py_NewRef(args[0]), Py_NewRef(args[1]), nullptr);
    return NULL;
}

#include "internal/pycore_frame.h"

static PyObject * stack_trace_impl(PyObject * module, PyObject * unused) {
    return retracesoftware::stacktrace_as_pyobject();
}

static PyObject* get_hashseed(PyObject* self, PyObject* args) {
    return PyLong_FromLong(_Py_HashSecret.expat.hashsalt);
}

static PyMethodDef module_methods[] = {
    {"hashseed", get_hashseed, METH_NOARGS, "Get PYTHONHASHSEED internals"},
    {"sigtrap", (PyCFunction)sigtrap, METH_O, "TODO"},
    {"stacktrace", (PyCFunction)stack_trace_impl, METH_NOARGS, "TODO"},
    {"type_flags", (PyCFunction)type_flags, METH_O, "return type flags as an int"},
    {"set_type_flags", (PyCFunction)set_type_flags, METH_VARARGS, "return type flags as an int"},
    {"noop", (PyCFunction)noop, METH_FASTCALL | METH_KEYWORDS, "TODO"},
    {"raise_exception", (PyCFunction)raise_exception, METH_FASTCALL, "TODO"},
    {"set_type", (PyCFunction)set_type, METH_VARARGS | METH_KEYWORDS, "TODO"},
    {NULL, NULL, 0, NULL}  // Sentinel
};

// Module definition
static PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "retracesoftware_utils",
    "TODO",
    0,
    module_methods
};

static PyObject * create_type_flags() {
    PyObject * flags = PyDict_New();

    #define ADD_Py_TPFLAG(d, x) PyDict_SetItemString(d, #x, PyLong_FromUnsignedLong(x))
    
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_MANAGED_DICT);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_SEQUENCE);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_MAPPING);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_DISALLOW_INSTANTIATION);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_IMMUTABLETYPE);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_HEAPTYPE);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_BASETYPE);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_HAVE_VECTORCALL);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_READY);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_READYING);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_HAVE_GC);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_HAVE_STACKLESS_EXTENSION);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_METHOD_DESCRIPTOR);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_VALID_VERSION_TAG);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_IS_ABSTRACT);
    ADD_Py_TPFLAG(flags, _Py_TPFLAGS_MATCH_SELF);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_LONG_SUBCLASS);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_LIST_SUBCLASS);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_TUPLE_SUBCLASS);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_BYTES_SUBCLASS);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_UNICODE_SUBCLASS);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_DICT_SUBCLASS);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_BASE_EXC_SUBCLASS);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_TYPE_SUBCLASS);
    ADD_Py_TPFLAG(flags, Py_TPFLAGS_DEFAULT);

    return flags;
}

// Module initialization
PyMODINIT_FUNC PyInit_retracesoftware_utils(void) {
    PyObject* module;

    // Create the module
    module = PyModule_Create(&moduledef);

    if (!module) {
        return NULL;
    }

    PyTypeObject * hidden_types[] = {
        &retracesoftware::ThreadStateWrapped_Type,
        &retracesoftware::Dispatch_Type,
        &retracesoftware::ThreadStateContext_Type,
        &retracesoftware::StableSetIterator_Type,
        nullptr
    };

    PyTypeObject * exposed_types[] = {
        // &retracesoftware::NullContext_Type,
        &retracesoftware::Counter_Type,
        &retracesoftware::BlockingCounter_Type,
        &retracesoftware::ThreadState_Type,

        &retracesoftware::Demultiplexer_Type,
        &retracesoftware::ThreadStatePredicate_Type,
        &retracesoftware::StripTraceback_Type,

        &retracesoftware::StableSet_Type,
        &retracesoftware::StableFrozenSet_Type,

        NULL
    };

    for (int i = 0; hidden_types[i]; i++) {
        PyType_Ready(hidden_types[i]);
    }

    for (int i = 0; exposed_types[i]; i++) {
        PyType_Ready(exposed_types[i]);

        // Find the last dot in the string
        const char *last_dot = strrchr(exposed_types[i]->tp_name, '.');

        // If a dot is found, the substring starts after the dot
        const char *name = (last_dot != NULL) ? (last_dot + 1) : exposed_types[i]->tp_name;

        PyModule_AddObject(module, name, (PyObject *)exposed_types[i]);
        // Py_DECREF(types[i]);
    }

    PyModule_AddObject(module, "TypeFlags", create_type_flags());

    return module;
}
