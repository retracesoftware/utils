#include "utils.h"
#include <exception>
#include <structmember.h>
#include <vector>
#include <condition_variable>
#include <mutex>
#include "unordered_dense.h"

using namespace ankerl::unordered_dense;

namespace retracesoftware {

    class PyGILGuard {
        PyGILState_STATE gstate_;
    public:
        PyGILGuard() {
            gstate_ = PyGILState_Ensure();
        }

        ~PyGILGuard() {
            PyGILState_Release(gstate_);
        }

        // Delete copy constructor and copy assignment
        PyGILGuard(const PyGILGuard&) = delete;
        PyGILGuard& operator=(const PyGILGuard&) = delete;

        // Allow move semantics (optional)
        PyGILGuard(PyGILGuard&& other) noexcept
            : gstate_(other.gstate_) {
            other.gstate_ = PyGILState_UNLOCKED; // or some sentinel
        }

        PyGILGuard& operator=(PyGILGuard&& other) noexcept {
            if (this != &other) {
                // Release current if held
                PyGILState_Release(gstate_);
                gstate_ = other.gstate_;
                other.gstate_ = PyGILState_UNLOCKED;
            }
            return *this;
        }
    };

    class WaitingContext {
        PyThreadState * save;
        PyObject * keys;
        PyObject * key;
        map<PyObject *, PyThreadState *> * pending;

    public:
        WaitingContext(
            PyObject * keys,
            map<PyObject *, PyThreadState *> &pending,
            PyObject * key)
        {
            this->keys = keys;
            this->pending = &pending;
            this->key = key;

            pending[key] = PyThreadState_Get();
            PySet_Add(keys, key);
            save = PyEval_SaveThread();
        }

        ~WaitingContext() {
            PyEval_RestoreThread(save);
            PySet_Discard(keys, key);
            pending->erase(key);
        }

        // Delete copy constructor and copy assignment
        WaitingContext(const WaitingContext&) = delete;
        WaitingContext& operator=(const WaitingContext&) = delete;
    };


    // when get is called, a key is presented
    // we dont need a lookahead
    // in the reader wrap the key into the stream, tuple returned

    struct Demultiplexer : public PyObject {

        PyObject * source;
        PyObject * key_function;
        PyObject * next;
        PyObject * next_key;
        PyObject * pending_keys;
        map<PyObject *, PyThreadState *> pending;
        vectorcallfunc vectorcall;
        std::mutex mtx;
        std::condition_variable wakeup;
        
        static int init(Demultiplexer * self, PyObject* args, PyObject* kwds) {

            PyObject * source;
            PyObject * key_function;

            static const char* kwlist[] = {"source", "key_function", nullptr};  // Keywords allowed

            if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO", (char **)kwlist, &source, &key_function)) {
                return -1;  
                // Return NULL to propagate the parsing error
            }
            
            self->source = Py_NewRef(source);
            self->key_function = Py_NewRef(key_function);
            self->next = self->next_key = nullptr;
            self->pending_keys = PySet_New(0);

            new (&self->pending) map<PyObject *, PyThreadState *>();
            self->vectorcall = (vectorcallfunc)call;

            new (&self->mtx) std::mutex();
            new (&self->wakeup) std::condition_variable();

            return 0;
        }

        bool wait(PyObject * key) {

            auto pred = [this, key]() {
                if (next_key == key) {
                    return true;
                }

                PyGILGuard gil;

                switch (PyObject_RichCompareBool(next_key, key, Py_EQ)) {
                    case 0: return false;
                    case 1: return true;
                    default:
                        throw std::exception();
                }
            };

            WaitingContext context(pending_keys, pending, key);

            std::unique_lock<std::mutex> lock(mtx);

            return wakeup.wait_for(lock, std::chrono::seconds(120), pred);
        }

        bool ensure_next() {
            
            if (!next) {                
                next = PyObject_CallNoArgs(source);
                if (!next) return false;

                next_key = PyObject_CallOneArg(key_function, next);

                if (!next_key) {
                    Py_DECREF(next);
                    return false;
                }
            }
            return true;
        }

        PyObject * get(PyObject * key) {

            // very first time through
            if (!ensure_next()) return nullptr;

            // fast path
            if (key == next_key) {
                assert(next);

                PyObject * res = next;
                
                next = PyObject_CallNoArgs(source);

                if (!next) {
                    Py_DECREF(res);
                    return nullptr;
                }

                next_key = PyObject_CallOneArg(key_function, next);
                Py_DECREF(key);

                if (next_key != key && !pending.empty()) {
                    wakeup.notify_all();
                }
                return res;

            } else if (PySet_Contains(pending_keys, key)) {
                PyErr_Format(PyExc_ValueError, "Key %S already in set of pending gets", key);
                return nullptr;
            } else {
                if (!next && !ensure_next()) {
                    return nullptr;
                }

                if (!wait(key)) {
                    raise(SIGTRAP);
                }
                PyObject * res = next;
                next = nullptr;

                if (!ensure_next()) {
                    Py_DECREF(res);
                    return nullptr;
                }

                if (!pending.empty()) {
                    wakeup.notify_all();
                }
                return res;
            }
        }

        static int traverse(Demultiplexer * self, visitproc visit, void* arg) {
            Py_VISIT(self->key_function);
            Py_VISIT(self->source);
            Py_VISIT(self->next);
            Py_VISIT(self->next_key);
            Py_VISIT(self->pending_keys);

            for (auto it = self->pending.begin(); it != self->pending.end(); it++) {
                Py_VISIT(it->first);
            }
            
            return 0;
        }

        static int clear(Demultiplexer * self) {
            Py_CLEAR(self->key_function);
            Py_CLEAR(self->source);
            Py_CLEAR(self->next);
            Py_CLEAR(self->next_key);
            Py_CLEAR(self->pending_keys);

            for (auto it = self->pending.begin(); it != self->pending.end(); it++) {
                Py_DECREF(it->first);
            }
            self->pending.clear();
            return 0;
        }

        static void dealloc(Demultiplexer *self) {
            PyObject_GC_UnTrack(self);          // Untrack from the GC
            clear(self);
            Py_TYPE(self)->tp_free(self);  // Free the object
        }

        static PyObject * call(Demultiplexer * self, PyObject* const * args, size_t nargsf, PyObject* kwnames) {
            if (kwnames || PyVectorcall_NARGS(nargsf) != 1) {
                PyErr_SetString(PyExc_TypeError, "demux take one positional argument, a predicate");
                return nullptr;
            }
            return self->get(args[0]);
        }
    };

    PyTypeObject Demultiplexer_Type = {
        .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = MODULE "demux",
        .tp_basicsize = sizeof(Demultiplexer),
        .tp_itemsize = 0,
        .tp_dealloc = (destructor)Demultiplexer::dealloc,
        .tp_vectorcall_offset = OFFSET_OF_MEMBER(Demultiplexer, vectorcall),
        // .tp_repr = (reprfunc)Gateway::tp_str,
        .tp_call = PyVectorcall_Call,
        // .tp_str = (reprfunc)Gateway::tp_str,
        .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_HAVE_VECTORCALL,
        .tp_doc = "TODO",
        .tp_traverse = (traverseproc)Demultiplexer::traverse,
        .tp_clear = (inquiry)Demultiplexer::clear,
        // .tp_methods = methods,
        // .tp_members = members,
        // .tp_dictoffset = OFFSET_OF_MEMBER(Gateway, dict),
        .tp_init = (initproc)Demultiplexer::init,
        .tp_new = PyType_GenericNew,
    };
}