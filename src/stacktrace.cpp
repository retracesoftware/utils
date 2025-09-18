#include "utils.h"
#include <structmember.h>
#include <vector>
#include <string>
#include <tuple>
#include <stdexcept>
#include <opcode.h>

#include "internal/pycore_frame.h"

namespace retracesoftware {

    // _PyInterpreterFrame * get_current_interpreter_frame(void)
    // {
    //     PyThreadState *tstate = PyThreadState_Get();
    //     if (!tstate || !tstate->cframe) {
    //         return NULL;
    //     }
    //     return tstate->cframe->current_frame;
    // }
        
    // PyObject * get_pending_c_callee_internal(_PyInterpreterFrame *frame)
    // {
    //     PyCodeObject *code = frame->f_code;  // new ref
    //     int lasti = _PyInterpreterFrame_LASTI(frame);

    //     if (!code) {
    //         Py_RETURN_NONE;
    //     }

    //     // Py_ssize_t codesize = PyBytes_Size(code->_co_code);
    //     // if (lasti < 0 || lasti >= codesize) {
    //     //     Py_DECREF(code);
    //     //     Py_RETURN_NONE;
    //     // }

    //     _Py_CODEUNIT bytecode = _PyCode_CODE(code)[lasti];
        
    //     int opcode = _Py_OPCODE(bytecode);
    //     int oparg = _Py_OPARG(bytecode);

    //     if (!(opcode == CALL || opcode == CALL_FUNCTION_EX)) {
    //         Py_DECREF(code);
    //         Py_RETURN_NONE;
    //     }
    //     // Internal frame access
    //     // _PyInterpreterFrame *iframe = (_PyInterpreterFrame *)frame;

    //     PyObject *callable = frame->localsplus[frame->stacktop - (oparg + 1)];
    //     if (PyCFunction_Check(callable)) {
    //         return Py_NewRef(callable);
    //     }
    //     Py_RETURN_NONE;
    // }

    static PyObject * get_self(PyFrameObject * frame) {
        PyObject *locals = PyFrame_GetLocals(frame);  // New reference
        
        if (!locals) {
            return nullptr;
        }
        assert(PyDict_Check(locals));

        PyObject *self = PyDict_GetItemString(locals, "self");
        Py_DECREF(locals);

        if (self) {
            return Py_NewRef(self);
        } else {
            Py_RETURN_NONE;
        }
    }

    static PyObject * get_class(PyFrameObject * frame) {
        PyObject * self = get_self(frame);
        if (!self || self == Py_None) return self;

        if (PyObject_HasAttrString(self, "__class__")) {
            PyObject *cls = PyObject_GetAttrString(self, "__class__");  // New reference
            Py_DECREF(self);

            if (cls) {
                return cls;
            } else {
                Py_RETURN_NONE;
            }
        } else {
            Py_DECREF(self);
            Py_RETURN_NONE;
        }
    }

    // static PyObject * classname(PyFrameObject * frame) {

    //     PyObject * cls = get_class(frame);
    //     if (!cls || cls == Py_None) return cls;

    //     PyObject *cls_name = PyObject_GetAttrString(cls, "__name__");  // New reference

    //     Py_DECREF(cls);

    //     if (cls_name) {
    //         return cls_name;
    //     } else {
    //         Py_RETURN_NONE;
    //     }

    //     // PyObject * self = get_self(frame);
    //     // if (!self) return nullptr;

    //     // if (PyObject_HasAttrString(self, "__class__")) {
    //     //     PyObject *cls = PyObject_GetAttrString(self, "__class__");  // New reference

    //     //     Py_DECREF(self);

    //     //     if (cls) {
    //     //         PyObject *cls_name = PyObject_GetAttrString(cls, "__name__");  // New reference
    //     //         Py_DECREF(cls);
    //     //         if (cls_name) {
    //     //             const char *cls_name_str = PyUnicode_AsUTF8(cls_name);
    //     //             if (cls_name_str && !PyErr_Occurred()) {
    //     //                     funcname = std::string(cls_name_str) + "." + funcname;
    //     //                 }
    //     //                 PyErr_Clear();
    //     //                 Py_DECREF(cls_name);
    //     //             }
    //     //             Py_DECREF(cls);
    //     //         }
    //     //     }
    //     //     Py_DECREF(locals);
    //     // }
    // }

    std::vector<Frame> stacktrace(void) {
        PyThreadState *tstate = PyThreadState_Get();
        if (!tstate) {
            throw std::runtime_error("No thread state!\n");
        }

        std::vector<Frame> frames;

        PyFrameObject *frame = PyThreadState_GetFrame(tstate);
        while (frame) {
            PyCodeObject *code = PyFrame_GetCode(frame);  // New reference
            int lineno = PyFrame_GetLineNumber(frame);

            const char *filename = PyUnicode_AsUTF8(code->co_filename);
            const char *funcname = PyUnicode_AsUTF8(code->co_name);
            
            PyObject *globals = PyFrame_GetGlobals(frame);

            const char * modulename = PyUnicode_AsUTF8(PyDict_GetItemString(globals, "__name__"));

            const char * classname = "";

            PyObject * cls = get_class(frame);
            
            if (cls && cls != Py_None) {
                PyObject * name = PyObject_GetAttrString(cls, "__name__");

                if (!name) throw nullptr;

                classname = PyUnicode_AsUTF8(name);
                Py_DECREF(name);
            }
            Py_XDECREF(cls);

            if (filename && funcname) {
                frames.push_back(std::make_tuple(std::string(filename), lineno, 
                    std::string(modulename),
                    std::string(classname),
                    std::string(funcname)));
            }
            
            Py_DECREF(code);
            Py_DECREF(globals);
            frame = PyFrame_GetBack(frame);
            // frame->f_back;
        }
        return frames;
    }


    // PyObject * foo1(void) {

    //     PyThreadState *tstate = PyThreadState_Get();
    //     if (!tstate) {
    //         throw std::runtime_error("No thread state!\n");
    //     }

    //     PyFrameObject *frame = PyThreadState_GetFrame(tstate);

    //     return get_pending_c_callee_internal(frame->f_frame);
    // }

    PyObject * stacktrace_as_pyobject(void) {

        try {
            std::vector<Frame> st = stacktrace();

            PyObject * res = PyList_New(st.size());

            if (!res) return nullptr;

            for (std::size_t i = 0; i < st.size(); ++i) {

                PyList_SetItem(res, i, PyTuple_Pack(5, PyUnicode_InternFromString(std::get<0>(st[i]).c_str()),
                                PyLong_FromLong(std::get<1>(st[i])), 
                                PyUnicode_InternFromString(std::get<2>(st[i]).c_str()),
                                PyUnicode_InternFromString(std::get<3>(st[i]).c_str()),
                                PyUnicode_InternFromString(std::get<4>(st[i]).c_str())));
            }
            return res;
        }
        catch (...) {
            return nullptr;
        }
    }
}