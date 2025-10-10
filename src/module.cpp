#include "utils.h"
#include <signal.h>

// static PyObject * set_type(PyObject * self, PyObject * args, PyObject *kwds) {

//     PyTypeObject * new_type;
//     PyObject * obj;

//     static const char *kwlist[] = {"obj", "type", NULL};  // List of keyword

//     if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO!", (char **)kwlist, &obj, &PyType_Type, &new_type)) {
//         return NULL;
//     }

//     if (!PyType_IsSubtype(new_type, Py_TYPE(obj))) {
//         PyErr_Format(PyExc_TypeError, "Cannot set type of object: %S to type: %S as not a subtype of object type: %S",
//             obj, (PyObject *)new_type, Py_TYPE(obj));
//         return nullptr;
//     }

//     if (new_type->tp_basicsize != Py_TYPE(obj)->tp_basicsize) {
//         PyErr_Format(PyExc_TypeError, "Cannot set type of object: %S to type: %S as memory layout differs",
//             obj, (PyObject *)new_type);
//         return nullptr;
//     }

//     Py_DECREF(Py_TYPE(obj));
//     Py_INCREF(new_type);
//     Py_SET_TYPE(obj, new_type);

//     Py_RETURN_NONE;
// }

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
    if (!s) {
        PyErr_Print();
        PyErr_Clear();
    } else {
        printf("%s\n", PyUnicode_AsUTF8(s));
        Py_DECREF(s);
    }
    // dump_stack_trace(PyThreadState_Get());
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

static PyObject* try_unwrap(PyObject* self, PyObject* arg) {
    return Py_NewRef(PyObject_TypeCheck(arg, &retracesoftware::Wrapped_Type)
        ? retracesoftware::Wrapped_Target(arg)
        : arg);
}

static PyObject* unwrap(PyObject* self, PyObject* arg) {
    if (!PyObject_TypeCheck(arg, &retracesoftware::Wrapped_Type)) {
        PyErr_Format(PyExc_TypeError, "Cannot unwrap: %S as it is not wrapped", arg);
        return nullptr;
    }
    return Py_NewRef(retracesoftware::Wrapped_Target(arg)); 
}

static PyObject * unwrap_apply(PyObject *module, PyObject * const * args, size_t nargs, PyObject* kwnames) {

    if (nargs == 0) {
        PyErr_SetString(PyExc_TypeError, "unwrap_apply requires at least one argument");
        return nullptr;
    }

    PyObject * wrapped = args[0];

    if (!PyObject_TypeCheck(wrapped, &retracesoftware::Wrapped_Type)) {
        PyErr_Format(PyExc_TypeError, "first argment: %S must be of type: %S", args[0], &retracesoftware::Wrapped_Type);
        return nullptr;
    }

    PyObject * result = PyObject_Vectorcall(
        retracesoftware::Wrapped_Target(wrapped),
        args + 1,
        (nargs - 1) | PY_VECTORCALL_ARGUMENTS_OFFSET,
        kwnames);

    return result;
}

static PyObject * yields_callable_instances(PyObject * module, PyObject * cls) {
    if (!PyType_Check(cls)) {
        PyErr_SetString(PyExc_TypeError, "yeilds_callable_instances takes a type");
        return nullptr;
    }

    return PyBool_FromLong(reinterpret_cast<PyTypeObject*>(cls)->tp_call != nullptr);
}

static PyObject * yields_weakly_referenceable_instances(PyObject * module, PyObject * cls) {

    if (!PyType_Check(cls)) {
        PyErr_SetString(PyExc_TypeError, "yields_weakly_referenceable_instances takes a type");
        return nullptr;
    }

    return PyBool_FromLong(reinterpret_cast<PyTypeObject*>(cls)->tp_weaklistoffset > 0);
}

static bool is_direct_subtype(PyTypeObject * sub, PyTypeObject * base) {
    if (sub == base) return true;
    else if (sub == nullptr) return false;
    else return is_direct_subtype(sub->tp_base, base);
}

static PyObject * set_type(PyObject * module, PyObject * const * args, size_t nargs) {

    if (nargs != 2) {
        PyErr_SetString(PyExc_TypeError, "set_type requires two arguments");
        return nullptr;
    }
    if (!PyType_Check(args[1])) {
        PyErr_SetString(PyExc_TypeError, "second argument to set_type must be a type");
        return nullptr;
    }

    PyTypeObject * target_type = reinterpret_cast<PyTypeObject *>(args[1]);
    PyTypeObject * current_type = Py_TYPE(args[0]);

    if (!is_direct_subtype(target_type, current_type)) {
        PyErr_Format(PyExc_TypeError, "target type: %S, must be subtype of: %S", target_type, current_type);
        return nullptr;
    }

    if (target_type->tp_itemsize != 0 || current_type->tp_itemsize != 0) {
        PyErr_Format(PyExc_TypeError, "assigning types where itemsize != 0 is not supported");
        return nullptr;
    }

    if (target_type->tp_basicsize != current_type->tp_basicsize) {
        PyErr_Format(PyExc_TypeError, "target type: %S, differs in size of subtype of: %S", target_type, current_type);
        return nullptr;
    }

    Py_DECREF(current_type);
    Py_INCREF(target_type);

    Py_SET_TYPE(args[0], target_type);

    Py_RETURN_NONE;
}

static PyObject * is_extendable(PyObject * module, PyObject * obj) {
    if (!PyType_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "is_extendable takes a type as a parameter");
        return nullptr;
    }

    PyTypeObject * cls = reinterpret_cast<PyTypeObject *>(obj);
    return PyBool_FromLong(PyType_HasFeature(cls, Py_TPFLAGS_BASETYPE));
}

static PyObject * is_immutable(PyObject * module, PyObject * obj) {
    if (!PyType_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "is_immutable takes a type as a parameter");
        return nullptr;
    }

    PyTypeObject * cls = reinterpret_cast<PyTypeObject *>(obj);
    return PyBool_FromLong(PyType_HasFeature(cls, Py_TPFLAGS_IMMUTABLETYPE));
}

static PyObject * create_wrapped(PyObject * module, PyObject * const * args, size_t nargs) {

    if (nargs != 2) {
        PyErr_SetString(PyExc_TypeError, "create_wrapped takes two parameters");
        return nullptr;
    }
    if (!PyType_Check(args[0])) {
        PyErr_Format(PyExc_TypeError, "first parameter of create_wrapped must be a type, but was passed: %S", args[0]);
        return nullptr;
    }

    return retracesoftware::create_wrapped(reinterpret_cast<PyTypeObject *>(args[0]), args[1]);
}

static PyObject * has_generic_new(PyObject * module, PyObject * obj) {
    if (!PyType_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "has_generic_new takes a type as a parameter");
        return nullptr;
    }
    PyTypeObject * cls = reinterpret_cast<PyTypeObject *>(obj);

    return PyBool_FromLong(cls->tp_new == PyType_GenericNew);
}

static PyObject * has_generic_alloc(PyObject * module, PyObject * obj) {
    if (!PyType_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "has_generic_alloc takes a type as a parameter");
        return nullptr;
    }
    PyTypeObject * cls = reinterpret_cast<PyTypeObject *>(obj);

    return PyBool_FromLong(cls->tp_alloc == PyType_GenericAlloc);
}

static PyObject * create_stub_object(PyObject * module, PyObject * obj) {
    if (!PyType_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "create_stub_object takes a type as a parameter");
        return nullptr;
    }
    PyTypeObject * cls = reinterpret_cast<PyTypeObject *>(obj);

    return cls->tp_alloc(cls, 0);
}

static PyObject * is_identity_hash(PyObject * module, PyObject * obj) {
    if (!PyType_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "is_identity_hash takes a type as a parameter");
        return nullptr;
    }
    PyTypeObject * cls = reinterpret_cast<PyTypeObject *>(obj);

    return PyBool_FromLong(cls->tp_hash == (hashfunc)_Py_HashPointer);
}

static PyObject * patch_hash(PyObject * module, PyObject * args, PyObject * kwargs) {

    PyTypeObject * cls;
    PyObject * func;
    static const char *kwlist[] = {"cls", "hashfunc", NULL};  // List of keyword

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O!O", (char **)kwlist, &PyType_Type, &cls, &func)) {
        return nullptr;
    }

    if (cls->tp_hash != (hashfunc)_Py_HashPointer) {
        PyErr_Format(PyExc_TypeError, "Not patching hash for class: %S as it does not have identity hash", cls);
        return nullptr;
    }
    retracesoftware::patch_hash(cls, func);
    Py_RETURN_NONE;
}

static PyObject * is_wrapped(PyObject * module, PyObject * obj) {
    return PyBool_FromLong(PyObject_TypeCheck(obj, &retracesoftware::Wrapped_Type));
}

static PyObject * is_method_descriptor(PyObject * module, PyObject * obj) {
    return PyBool_FromLong(PyType_HasFeature(Py_TYPE(obj), Py_TPFLAGS_METHOD_DESCRIPTOR));
}

static PyObject * thread_id(PyObject * module, PyObject * unused) {
    PyObject * id = PyDict_GetItem(PyThreadState_GetDict(), module);

    return Py_NewRef(id ? id : Py_None);
}

static PyObject * set_thread_id(PyObject * module, PyObject * id) {

    if (PyDict_SetItem(PyThreadState_GetDict(), module, Py_NewRef(id)) == -1) {
        Py_DECREF(id);        
        return nullptr;
    }
    Py_RETURN_NONE;
}

static PyObject * intercept_frame_eval(PyObject * module, PyObject * args, PyObject * kwargs) {
    PyObject * on_call = nullptr;
    PyObject * on_result = nullptr;
    PyObject * on_error = nullptr;

    static const char *kwlist[] = {"on_call", "on_result", "on_error", nullptr};  // List of keyword

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|OOO", (char **)kwlist, &on_call, &on_result, &on_error)) {
        return nullptr;
    }

    PyInterpreterState * is = PyInterpreterState_Get();

    retracesoftware::FrameEval_Install(is, on_call, on_result, on_error);

    Py_RETURN_NONE;
}

static PyObject * remove_frame_eval(PyObject * module, PyObject * unused) {
    retracesoftware::FrameEval_Remove(PyInterpreterState_Get());
    Py_RETURN_NONE;
} 

static PyObject * make_compatible_subtype(PyTypeObject * base) {

    /* Build a type spec with no new fields/slots, but with base's alloc/free. */
    PyType_Slot slots[] = {
        /* Inherit most behavior; just wire alloc/free to match base. */
        {Py_tp_alloc, (void *)base->tp_alloc},
        {Py_tp_free,  (void *)base->tp_free},
        /* Optionally, inherit tp_new explicitly (often unnecessary). */
        {Py_tp_new,   (void *)(base->tp_new ? base->tp_new : PyType_GenericNew)},
        {0, 0}
    };

    PyType_Spec spec = {
        .name = base->tp_name,                           /* e.g. "m.CompatSub" if you want a qualname */
        .basicsize = (int)base->tp_basicsize,                 /* no extra fields */
        .itemsize  = (int)base->tp_itemsize,                  /* preserve var-sized layout if any */
        .flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_BASETYPE,
                    //  (base->tp_flags & Py_TPFLAGS_HAVE_GC)  /* keep GC tracking consistent */
        .slots = slots
    };

    PyObject *bases = PyTuple_Pack(1, (PyObject *)base);
    if (!bases) return NULL;

    PyObject *sub = PyType_FromSpecWithBases(&spec, bases);
    Py_DECREF(bases);
    return sub;   /* New ref or NULL on error */
}

static PyObject * extend_type(PyObject * module, PyObject * obj) {

    if (!PyType_Check(obj)) {
        PyErr_SetString(PyExc_TypeError, "patch_new takes a type as a parameter");
        return nullptr;
    }
    PyTypeObject * cls = reinterpret_cast<PyTypeObject *>(obj);

    return make_compatible_subtype(cls);
}

static PyObject * intercept__new__(PyObject * module, PyObject * args, PyObject *kwargs) {

    PyTypeObject * cls;
    PyObject * handler;
    static const char *kwlist[] = {"type", "handler", NULL};  // List of keyword

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O!O", (char **)kwlist, &PyType_Type, &cls, &handler)) {
        return nullptr;
    }

    if (!retracesoftware::install_new_wrapper(cls, handler)) {
        return nullptr;
    }
    Py_RETURN_NONE;
}

static PyMethodDef module_methods[] = {
    {"intercept_frame_eval", (PyCFunction)intercept_frame_eval, METH_VARARGS | METH_KEYWORDS, "Tests if the given type has an identity hash"},
    {"remove_frame_eval", (PyCFunction)remove_frame_eval, METH_NOARGS, "Tests if the given type has an identity hash"},
    {"intercept__new__", (PyCFunction)intercept__new__, METH_VARARGS | METH_KEYWORDS, "TODO"},
    {"extend_type", extend_type, METH_O, "TODO"},
    {"patch_hash", (PyCFunction)patch_hash, METH_VARARGS | METH_KEYWORDS, "Tests if the given type has an identity hash"},
    {"is_identity_hash", is_identity_hash, METH_O, "Tests if the given type has an identity hash"},
    {"thread_id", (PyCFunction)thread_id, METH_NOARGS, "TODO"},
    {"set_thread_id", (PyCFunction)set_thread_id, METH_O, "TODO"},
    {"is_method_descriptor", is_method_descriptor, METH_O, "Returns if the object is a method descriptor"},
    {"is_wrapped", is_wrapped, METH_O, "Returns if the object is wrapped"},
    {"create_stub_object", create_stub_object, METH_O, "Creates a stub object given the type, but bypasses __new__ and __init__ initialization"},
    {"has_generic_new", has_generic_new, METH_O, "Does the supplied type have a generic __new__ function?"},
    {"has_generic_alloc", has_generic_alloc, METH_O, "Does the supplied type have a generic allocator"},
    {"is_extendable", is_extendable, METH_O, "Is the supplied type is extendable"},
    {"is_immutable", is_immutable, METH_O, "Is the supplied type is immutable"},
    {"create_wrapped", (PyCFunction)create_wrapped, METH_FASTCALL, "TODO"},
    {"set_type", (PyCFunction)set_type, METH_FASTCALL, "TODO"},
    {"yields_callable_instances", (PyCFunction)yields_callable_instances, METH_O, "TODO"},
    {"yields_weakly_referenceable_instances", (PyCFunction)yields_weakly_referenceable_instances, METH_O, "TODO"},
    {"unwrap_apply", (PyCFunction)unwrap_apply, METH_FASTCALL | METH_KEYWORDS, "TODO"},
    {"try_unwrap", try_unwrap, METH_O, "TODO"},
    {"unwrap", unwrap, METH_O, "TODO"},
    {"hashseed", get_hashseed, METH_NOARGS, "Get PYTHONHASHSEED internals"},
    {"sigtrap", (PyCFunction)sigtrap, METH_O, "TODO"},
    {"stacktrace", (PyCFunction)stack_trace_impl, METH_NOARGS, "TODO"},
    {"type_flags", (PyCFunction)type_flags, METH_O, "return type flags as an int"},
    {"set_type_flags", (PyCFunction)set_type_flags, METH_VARARGS, "return type flags as an int"},
    {"noop", (PyCFunction)noop, METH_FASTCALL | METH_KEYWORDS, "TODO"},
    {"raise_exception", (PyCFunction)raise_exception, METH_FASTCALL, "TODO"},
    // {"set_type", (PyCFunction)set_type, METH_VARARGS | METH_KEYWORDS, "TODO"},
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
        &retracesoftware::IdSetTest_Type,
        &retracesoftware::IdSetLogical_Type,
        &retracesoftware::WeakRefCallback_Type,
        &retracesoftware::FrameEval_Type,
        &retracesoftware::CurrentFrame_Type,
        &retracesoftware::NewWrapper_Type,
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

        &retracesoftware::FastSet_Type,
        &retracesoftware::InstanceCheck_Type,
        &retracesoftware::Visitor_Type,
        &retracesoftware::Wrapped_Type,
        &retracesoftware::Proxy_Type,
        &retracesoftware::WrappedFunction_Type,
        &retracesoftware::Reference_Type,
        &retracesoftware::ThreadSwitchMonitor_Type,
        &retracesoftware::IdSet_Type,
        &retracesoftware::IdDict_Type,
        &retracesoftware::StripTraceback_Type,
        &retracesoftware::Observer_Type,
        &retracesoftware::PerThread_Type,
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
