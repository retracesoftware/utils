#include "utils.h"
#include <structmember.h>

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

    struct FrameEval;
    struct CurrentFrame;

    static thread_local PyThreadState * cached_thread = nullptr;
    static thread_local CurrentFrame * cached_current = nullptr;

    static thread_local PyInterpreterState * cached_state = nullptr;
    static thread_local FrameEval * cached_eval = nullptr;

    static thread_local bool in_callback = false;
    static PyObject * interpreter_key = nullptr;
    static PyObject * kwnames = nullptr;

    static FrameEval * find_frame_eval(PyInterpreterState * is) {
        if (is != cached_state) {
            PyObject * dict = PyInterpreterState_GetDict(is);
            
            cached_eval = (FrameEval *)PyDict_GetItem(dict, interpreter_key);
        }
        return cached_eval;
    }

    static CurrentFrame * find_current_frame() {

        if (cached_thread != PyThreadState_Get()) {
            PyObject * dict = PyThreadState_GetDict();
            cached_current = (CurrentFrame *)PyDict_GetItem(dict, interpreter_key);

            if (!cached_current) {
                cached_current = (CurrentFrame *)CurrentFrame_Type.tp_alloc(&CurrentFrame_Type, 0);
                
                PyDict_SetItem(dict, interpreter_key, (PyObject *)cached_current);
                Py_DECREF(cached_current);
            }
        }

        return cached_current;
    }

    struct CurrentFrame : public PyObject {
        struct _PyInterpreterFrame * frame;

        static PyGetSetDef getset[];
        
        static PyObject * function_getter(CurrentFrame *self, void *closure) {
            return Py_NewRef((PyObject *)self->frame->f_func);
        }

        static PyObject * locals_getter(CurrentFrame *self, void *closure) {

            PyObject * locals = PyDict_New();

            PyObject * names = self->frame->f_code->co_localsplusnames;

            for (size_t i = 0; i < PyTuple_Size(names); i++) {
                PyObject * local = self->frame->localsplus[i];
                PyDict_SetItem(locals, PyTuple_GetItem(names, i), local ? local : Py_None);
            }
            return locals;
        }

        static PyObject * globals_getter(CurrentFrame *self, void *closure) {
            return Py_NewRef(self->frame->f_globals ? self->frame->f_globals : Py_None);
        }

        // = {
        //     // {"pending_keys", (getter)Demultiplexer::pending_getter, nullptr, "TODO", NULL},
        //     {NULL}  // Sentinel
        // };

        static PyMemberDef members[];
        //  = {
        //     // {"pending", T_OBJECT, OFFSET_OF_MEMBER(Demultiplexer, next), READONLY, "TODO"},
        //     {NULL}  /* Sentinel */
        // };
    };

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

    struct FrameEval : public PyObject {
        _PyFrameEvalFunction frame_eval = nullptr;
        FastCall on_call;
        FastCall on_result;
        FastCall on_error;
        
        static int traverse(FrameEval* self, visitproc visit, void* arg) {
            Py_VISIT(self->on_call.callable);
            Py_VISIT(self->on_result.callable);
            Py_VISIT(self->on_error.callable);
            return 0;
        }

        static int clear(FrameEval* self) {
            Py_CLEAR(self->on_call.callable);
            Py_CLEAR(self->on_result.callable);
            Py_CLEAR(self->on_error.callable);
            return 0;
        }

        static void dealloc(FrameEval* self) {
            PyObject_GC_UnTrack(self);
            clear(self);
            Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
        }
    };

    static PyObject* wrapper(PyThreadState *tstate, struct _PyInterpreterFrame * frame, int throw_flag) {

        FrameEval * eval = find_frame_eval(PyThreadState_GetInterpreter(tstate));

        if (in_callback || throw_flag) {
            return eval->frame_eval(tstate, frame, throw_flag);
        } else {
            PyObject * context;

            in_callback = true;

            if (eval->on_call.callable) {

                CurrentFrame * current = find_current_frame();

                current->frame = frame;

                context = eval->on_call(current);

                if (!context) {
                    PyErr_Clear();
                    PyObject * result = eval->frame_eval(tstate, frame, throw_flag);
                    in_callback = false;
                    return result;
                }

            } else {
                context = Py_NewRef(Py_None);
            }

            in_callback = false;

            PyObject * result = eval->frame_eval(tstate, frame, throw_flag);

            in_callback = true;

            if (result) {
                if (eval->on_result.callable) {
                    PyObject * r = eval->on_result(context, result);

                    if (!r) {
                        Py_DECREF(result);
                        Py_DECREF(context);
                        return nullptr;
                    }
                    Py_DECREF(r);
                }
            } else {
                if (eval->on_error.callable) {
                    assert (PyErr_Occurred());

                    PyObject * exc[] = {context, nullptr, nullptr, nullptr};

                    // Fetch the current exception
                    PyErr_Fetch(exc + 1, exc + 2, exc + 3);

                    for (int i = 1; i < 4; i++) if (!exc[i]) exc[i] = Py_None;

                    PyObject * r = eval->on_error(exc, 4, nullptr);

                    if (!r) {
                        for (int i = 1; i < 4; i++) if (exc[i] != Py_None) Py_DECREF(exc[i]);
                    } else {
                        Py_DECREF(r);
                        PyErr_Restore(
                            exc[1] == Py_None ? nullptr : exc[1], 
                            exc[2] == Py_None ? nullptr : exc[2],
                            exc[3] == Py_None ? nullptr : exc[3]);
                    }
                }
            }
            in_callback = false;
            Py_DECREF(context);
            return result;
        }
    }

    PyTypeObject CurrentFrame_Type = {
        .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = MODULE "CurrentFrame",
        .tp_basicsize = sizeof(CurrentFrame),
        .tp_itemsize = 0,
        // .tp_dealloc = (destructor)CurrentFrame::dealloc,
        .tp_flags = Py_TPFLAGS_DEFAULT,
        .tp_doc = "TODO",
        .tp_members = CurrentFrame::members,
        .tp_getset = CurrentFrame::getset,
    };

    PyTypeObject FrameEval_Type = {
        .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = MODULE "FrameEval",
        .tp_basicsize = sizeof(FrameEval),
        .tp_itemsize = 0,
        .tp_dealloc = (destructor)FrameEval::dealloc,
        .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
        .tp_doc = "TODO",
        .tp_traverse = (traverseproc)FrameEval::traverse,
        .tp_clear = (inquiry)FrameEval::clear,
    };

    void FrameEval_Remove(PyInterpreterState * is) {
        PyObject * dict = PyInterpreterState_GetDict(is);

        cached_state = nullptr;
        cached_eval = nullptr;

        if (!interpreter_key) interpreter_key = PyUnicode_InternFromString("__retrace__");

        PyObject * eval = PyDict_GetItem(dict, interpreter_key);

        if (eval) {
            _PyInterpreterState_SetEvalFrameFunc(is, ((FrameEval *)eval)->frame_eval);
            PyDict_DelItem(dict, interpreter_key);
        }
    }

    bool FrameEval_Install(
        PyInterpreterState * is, 
        PyObject * on_call,
        PyObject * on_result,
        PyObject * on_error) {

        FrameEval_Remove(is);

        FrameEval * eval = (FrameEval *)FrameEval_Type.tp_alloc(&FrameEval_Type, 0);

        if (!eval) {
            return false;
        }

        eval->on_call = on_call ? FastCall(Py_NewRef(on_call)) : FastCall();
        eval->on_result = on_result ? FastCall(Py_NewRef(on_result)) : FastCall();
        eval->on_error = on_error ? FastCall(Py_NewRef(on_error)) : FastCall();
        eval->frame_eval = _PyInterpreterState_GetEvalFrameFunc(is);

        PyObject * dict = PyInterpreterState_GetDict(is);

        // if (!interpreter_key) interpreter_key = PyUnicode_InternFromString("__retrace__");
        if (!kwnames) kwnames = Py_BuildValue("(s,s,s)", "func", "globals", "locals");
    
        _PyInterpreterState_SetEvalFrameFunc(is, wrapper);

        PyDict_SetItem(dict, interpreter_key, eval);

        Py_DECREF(eval);
        return true;
    }
}