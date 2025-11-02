#include "utils.h"
#include <structmember.h>
#include <thread>

typedef struct _PyInterpreterFrame {
    /* "Specials" section */
    PyFunctionObject *f_func; /* Strong reference */
    PyObject *f_globals; /* Borrowed reference */
    PyObject *f_builtins; /* Borrowed reference */
    PyObject *f_locals; /* Strong reference, may be NULL */
    PyCodeObject *f_code; /* Strong reference */
    PyFrameObject *frame_obj; /* Strong reference, may be NULL */
    /* Linkage section */
    struct _PyInterpreterFrame *previous;
    // NOTE: This is not necessarily the last instruction started in the given
    // frame. Rather, it is the code unit *prior to* the *next* instruction. For
    // example, it may be an inline CACHE entry, an instruction we just jumped
    // over, or (in the case of a newly-created frame) a totally invalid value:
    _Py_CODEUNIT *prev_instr;
    int stacktop;     /* Offset of TOS from localsplus  */
    bool is_entry;  // Whether this is the "root" frame for the current _PyCFrame.
    char owner;
    /* Locals and stack */
    PyObject *localsplus[1];
} _PyInterpreterFrame;

namespace retracesoftware {

    struct CurrentFrame;

    static PyObject* wrapper(PyThreadState *tstate, struct _PyInterpreterFrame * frame, int throw_flag);

    static PyObject * key() {
            static PyObject * name = nullptr;
            if (!name) name = PyUnicode_InternFromString("__retrace_utils_eval_wrapper__");
            return name;
        }

    struct CurrentFrame : public PyObject {
        PyObject * eval;
        struct _PyInterpreterFrame * frame;
        PyThreadState * tstate;
        PyObject * callback;
        // vectorcallfunc vectorcall;

        static thread_local std::pair<PyThreadState *, CurrentFrame *> cached;

        static CurrentFrame * find(PyThreadState * tstate) {
            if (cached.first != tstate) {
                PyObject * current = PyDict_GetItem(_PyThreadState_GetDict(tstate), key());

                if (!current) {
                    current = CurrentFrame_Type.tp_alloc(&CurrentFrame_Type, 0);
                    if (!current) return nullptr;
            
                    // current->vectorcall = (vectorcallfunc)CurrentFrame::call_target;
                    ((CurrentFrame *)current)->callback = nullptr;

                    PyInterpreterState * is =PyThreadState_GetInterpreter(tstate);

                    PyObject * eval = PyDict_GetItemString(PyInterpreterState_GetDict(is), "__retrace__");
                    ((CurrentFrame *)current)->eval = Py_NewRef(eval);
                    ((CurrentFrame *)current)->tstate = tstate;
                }
                if (Py_TYPE(current) != &CurrentFrame_Type) {
                    PyErr_Format(PyExc_TypeError, "Internal retrace error, Current frame in thread dict: %S, was not of expected type: %S", current, &CurrentFrame_Type);
                    return nullptr;
                }
                cached.first = tstate;
                cached.second = (CurrentFrame *)current;

            }
            return cached.second;
        }

        _PyFrameEvalFunction evalfunc() {
            return (_PyFrameEvalFunction)PyCapsule_GetPointer(eval, nullptr);
        }

        bool handle_callback_result(PyObject * result) {
            if (!result) {
                if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                    PyErr_Clear();
                    return true;
                } else {
                    return false;
                }
            }
            Py_DECREF(result);
            return true;
        }

        bool call_result_callback(PyObject * callback, PyObject * result) {
            static PyObject * method = nullptr;
            if (!method) method = PyUnicode_InternFromString("on_result");

            assert (callback != Py_None);
            return handle_callback_result(PyObject_CallMethodOneArg(callback, method, result));
        }

        bool call_error_callback(PyObject * callback, PyObject *exc_type, PyObject *exc_value, PyObject *exc_traceback) {
            static PyObject * method = nullptr;
            if (!method) method = PyUnicode_InternFromString("on_error");

            assert (callback != Py_None);

            PyObject * res = PyObject_CallMethodObjArgs(callback, method, 
                exc_type ? exc_type : Py_None, 
                exc_value ? exc_value : Py_None, 
                exc_traceback ? exc_traceback : Py_None,
                nullptr);

            return handle_callback_result(res);
        }

        bool call_return_callback(PyObject * callback) {
            static PyObject * method = nullptr;
            if (!method) method = PyUnicode_InternFromString("on_return");

            assert (callback != Py_None);

            return handle_callback_result(PyObject_CallMethodNoArgs(callback, method));
        }

        PyObject * handle_return(PyObject * callback, PyObject * result) {
            if (callback == Py_None) return result;

            if (result) {    
                if (!call_return_callback(callback)) return nullptr;

                static PyObject * method = nullptr;
                if (!method) method = PyUnicode_InternFromString("on_result");

                PyObject * new_result = PyObject_CallMethodOneArg(callback, method, result);

                if (new_result) {
                    Py_DECREF(result);
                    return new_result;
                } else {
                    if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                        PyErr_Clear();
                        return result;
                    } else {
                        Py_DECREF(result);
                        return nullptr;
                    }
                }
            } else {
                PyObject * exc[] = {nullptr, nullptr, nullptr};
                // Fetch the current exception
                PyErr_Fetch(exc + 0, exc + 1, exc + 2);

                if (!call_return_callback(callback)) {
                    for (int i = 0; i < 3; i++) Py_XDECREF(exc[i]);
                    return nullptr;
                }
                
                if (!call_return_callback(callback) ||
                    !call_error_callback(callback, exc[0], exc[1], exc[2])) {
                    for (int i = 0; i < 3; i++) Py_XDECREF(exc[i]);
                    return nullptr;
                }
                PyErr_Restore(exc[0], exc[1], exc[2]);
                return nullptr;
            }
        }

        // void call_callback() {
        //     PyObject * saved_callback = callback;
        //     callback = nullptr;
        //     callback = PyObject_CallOneArg(saved_callback, this);
        // }

        PyObject * call_from_intercept(_PyInterpreterFrame * frame, int throwflag) {

            // if (tstate->tracing) {
            //     PyInterpreterState * is = PyThreadState_GetInterpreter(tstate);
            //     _PyFrameEvalFunction saved_func = _PyInterpreterState_GetEvalFrameFunc(is);
            //     _PyInterpreterState_SetEvalFrameFunc(is, _PyEval_EvalFrameDefault);
            //     PyObject * result = _PyEval_EvalFrameDefault(tstate, frame, throwflag);
            //     _PyInterpreterState_SetEvalFrameFunc(is, saved_func);
            //     return result;
            // }

            _PyFrameEvalFunction func = evalfunc();

            if (!func) {
                assert(PyErr_Occurred());
                return nullptr;
            }

            if (!tstate->tracing && !throwflag && callback && PyCallable_Check(callback)) {
                this->frame = frame;

                PyObject * current_callback = callback;
                callback = nullptr;

                PyObject * new_callback = PyObject_CallOneArg(current_callback, this);
                
                if (!new_callback) {
                    assert(PyErr_Occurred());
                    callback = current_callback;
                    return nullptr;    
                }

                // assert (Py_TYPE(new_callback) != &CurrentFrame_Type);

                callback = new_callback;
                PyObject * result;
                            
                if (!PyCallable_Check(callback) && Py_REFCNT(eval) == 2) {
                    PyInterpreterState * is = PyThreadState_GetInterpreter(tstate);
                    _PyFrameEvalFunction saved_func = _PyInterpreterState_GetEvalFrameFunc(is);

                    _PyInterpreterState_SetEvalFrameFunc(is, func);
                    result = func(tstate, frame, throwflag);
                    _PyInterpreterState_SetEvalFrameFunc(is, saved_func);
                } else {
                    result = func(tstate, frame, throwflag);
                }

                callback = nullptr;
                this->frame = frame;
                
                result = handle_return(new_callback, result);
                Py_DECREF(new_callback);

                callback = current_callback;
                return result;

            } else {

                return func(tstate, frame, throwflag);
            }
        }

        static PyGetSetDef getset[];
        
        static PyObject * function_getter(CurrentFrame *self, void *closure) {
            return Py_NewRef((PyObject *)self->frame->f_func);
        }

        static PyObject * locals_getter(CurrentFrame *self, void *closure) {

            PyObject * locals = PyDict_New();

            PyObject * names = self->frame->f_code->co_localsplusnames;

            for (Py_ssize_t i = 0; i < PyTuple_Size(names); i++) {
                PyObject * local = self->frame->localsplus[i];
                PyDict_SetItem(locals, PyTuple_GetItem(names, i), local ? local : Py_None);
            }
            return locals;
        }

        static PyObject * globals_getter(CurrentFrame *self, void *closure) {
            return Py_NewRef(self->frame->f_globals ? self->frame->f_globals : Py_None);
        }

        static int traverse(CurrentFrame * self, visitproc visit, void* arg) {
            Py_VISIT(self->callback);
            Py_VISIT(self->eval);
            return 0;
        }

        static int clear(CurrentFrame * self) {
            Py_CLEAR(self->callback);
            Py_CLEAR(self->eval);
            return 0;
        }

        static void dealloc(CurrentFrame *self) {
            PyObject_GC_UnTrack(self);          // Untrack from the GC
            clear(self);
            Py_TYPE(self)->tp_free(self);  // Free the object
        }

        static PyMemberDef members[];
    };

    thread_local std::pair<PyThreadState *, CurrentFrame *> CurrentFrame::cached;

    PyGetSetDef CurrentFrame::getset[] = {
        {"function", (getter)CurrentFrame::function_getter, nullptr, "TODO", NULL},
        {"locals", (getter)CurrentFrame::locals_getter, nullptr, "TODO", NULL},
        {"globals", (getter)CurrentFrame::globals_getter, nullptr, "TODO", NULL},
        {NULL}
    };

    PyMemberDef CurrentFrame::members[] = {
        // {"pending", T_OBJECT, OFFSET_OF_MEMBER(Demultiplexer, next), READONLY, "TODO"}
        {NULL}
    };

    // ensure that the PyInterpreterState_Dict has a FrameEval, 
    // its ref-count should be 1, in this scenario. It is NOT installed at this point
    struct FrameEval : public PyObject {
        _PyFrameEvalFunction frame_eval = nullptr;
    };

    static PyObject* wrapper(PyThreadState *tstate, struct _PyInterpreterFrame * frame, int throw_flag) {
        CurrentFrame * current = CurrentFrame::find(tstate);
        if (!current) return nullptr;
        assert(current->tstate == tstate);

        return current->call_from_intercept(frame, throw_flag);
    }

    PyTypeObject CurrentFrame_Type = {
        .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = MODULE "CurrentFrame",
        .tp_basicsize = sizeof(CurrentFrame),
        .tp_itemsize = 0,
        .tp_dealloc = (destructor)CurrentFrame::dealloc,
        // .tp_vectorcall_offset = OFFSET_OF_MEMBER(CurrentFrame, vectorcall),
        // .tp_call = PyVectorcall_Call,
        .tp_flags = Py_TPFLAGS_DEFAULT,
        .tp_doc = "TODO",
        .tp_traverse = (traverseproc)CurrentFrame::traverse,
        .tp_clear = (inquiry)CurrentFrame::clear,
        .tp_members = CurrentFrame::members,
        .tp_getset = CurrentFrame::getset,
    };

    // static PyObject * sys_setprofile(PyObject *self, PyObject *args)
    // have something which disable frame eval, can wrap a target callable

    bool FrameEval_Install(PyInterpreterState * is, PyObject * handler) {

        const char * name =  "__retrace__";

        PyObject * eval = (FrameEval *)PyDict_GetItemString(PyInterpreterState_GetDict(is), name);

        if (!eval) {
            eval = PyCapsule_New((void *)_PyInterpreterState_GetEvalFrameFunc(is), nullptr, nullptr);
            if (!eval) return false;

            PyDict_SetItemString(PyInterpreterState_GetDict(is), name, eval);
            Py_DECREF(eval);
        }

        if (handler == Py_None) {
            PyDict_DelItem(PyThreadState_GetDict(), key());
            if (Py_REFCNT(eval) == 1) {
                _PyFrameEvalFunction func = (_PyFrameEvalFunction)PyCapsule_GetPointer(eval, nullptr);
                _PyInterpreterState_SetEvalFrameFunc(is, func);
            }
            return true;
            
        } else if (!PyCallable_Check(handler)) {
            PyErr_Format(PyExc_TypeError, "handler: %S must be None or callable", handler);
            return false;

        } else {

            _PyInterpreterState_SetEvalFrameFunc(is, wrapper);

            CurrentFrame * current = CurrentFrame::find(PyThreadState_Get());

            if (!current) return false;
            
            Py_XDECREF(current->callback);
            current->callback = Py_NewRef(handler);

            assert(PyCapsule_GetPointer(eval, nullptr) != nullptr);

            current->frame = nullptr;

            PyDict_SetItem(PyThreadState_GetDict(), key(), current);
            return true;
        }
    }
}