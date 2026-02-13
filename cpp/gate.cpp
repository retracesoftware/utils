#include "utils.h"
#include "alloca_compat.h"
#include <string.h>

namespace retracesoftware {

    struct Gate;
    struct BoundGate;
    struct GateContext;
    struct ApplyWith;
    struct GatePredicate;

    extern PyTypeObject Gate_Type;
    extern PyTypeObject BoundGate_Type;
    extern PyTypeObject GateContext_Type;
    extern PyTypeObject ApplyWith_Type;
    extern PyTypeObject GatePredicate_Type;

    // ========================================================================
    // Gate — a single object with a thread-local executor slot.
    //
    // The executor is a Python callable: executor(target, *args, **kwargs)
    // When NULL (disabled), bound wrappers call the target directly.
    //
    // Backing store: PyThreadState_GetDict() keyed by the Gate object.
    // This is the durable, per-thread source of truth.
    //
    // Fast path: a plain global cache (not thread_local). With the GIL,
    // only one thread runs at a time, so a global cache is coherent
    // without synchronization. Thread switches are detected by comparing
    // PyThreadState_Get() against the cached thread state — a cheap
    // pointer comparison. With a single global Gate, the typical hot path
    // is two pointer comparisons + one null check + one indirect call. No TLS access at all.
    // ========================================================================

    struct GateCache {
        PyThreadState * tstate;  // thread that populated this cache
        Gate * gate;             // gate this cache is for
        FastCall executor;       // cached executor (callable == NULL ⇒ disabled)
    };

    // Plain global — no thread_local, no TLS overhead.
    // Safe because only one thread runs at a time under the GIL.
    static GateCache cache = {nullptr, nullptr, {}};

    struct Gate : public PyObject {
        vectorcallfunc vectorcall;
        PyObject * default_executor;  // used when thread dict has no entry; NULL = disabled

        // Slow path: load this gate's executor from the current thread's dict
        // into the global cache. Marked noinline to keep the hot path tight.
        // When no dict entry exists, falls back to default_executor.
        __attribute__((noinline))
        void load_cache(PyThreadState * tstate) {
            PyObject * dict = PyThreadState_GetDict();
            PyObject * exec = PyDict_GetItem(dict, (PyObject *)this);  // borrowed ref
            if (!exec && this->default_executor) {
                exec = this->default_executor;
            }
            cache.tstate = tstate;
            cache.gate = this;
            cache.executor = exec ? FastCall(exec) : FastCall();
        }

        // Get the current thread's executor (borrowed reference, may be NULL).
        // Hot path: two pointer comparisons against the global cache.
        inline PyObject * executor() {
            PyThreadState * tstate = PyThreadState_Get();
            if (__builtin_expect(cache.tstate != tstate || cache.gate != this, 0)) {
                load_cache(tstate);
            }
            return cache.executor.callable;
        }

        void set_executor(PyObject * exec) {
            PyThreadState * tstate = PyThreadState_Get();
            PyObject * dict = PyThreadState_GetDict();

            if (exec) {
                PyDict_SetItem(dict, (PyObject *)this, exec);
                cache.executor = FastCall(exec);
            } else {
                PyDict_DelItem(dict, (PyObject *)this);
                PyErr_Clear();
                // Clearing override: use default if set, else disabled
                cache.executor = this->default_executor ? FastCall(this->default_executor) : FastCall();
            }

            // Update cache identity
            cache.tstate = tstate;
            cache.gate = this;
        }

        // --- Python methods ---

        static int init(Gate * self, PyObject * args, PyObject * kwds) {
            self->default_executor = nullptr;
            static const char * kwlist[] = {"default", NULL};
            PyObject * default_exec = nullptr;
            if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O:Gate", (char **)kwlist, &default_exec)) {
                return -1;
            }
            if (default_exec != nullptr && default_exec != Py_None) {
                if (!PyCallable_Check(default_exec)) {
                    PyErr_Format(PyExc_TypeError, "Gate default must be callable or None, got %S", default_exec);
                    return -1;
                }
                self->default_executor = Py_NewRef(default_exec);
            }
            self->vectorcall = (vectorcallfunc)Gate::call;
            return 0;
        }

        static PyObject * set(Gate * self, PyObject * executor) {
            if (executor == Py_None) {
                self->set_executor(nullptr);
            } else {
                if (!PyCallable_Check(executor)) {
                    PyErr_Format(PyExc_TypeError, "executor must be callable, got %S", executor);
                    return nullptr;
                }
                self->set_executor(executor);
            }
            Py_RETURN_NONE;
        }

        static PyObject * disable(Gate * self, PyObject * Py_UNUSED(args)) {
            self->set_executor(nullptr);
            Py_RETURN_NONE;
        }

        static PyObject * is_set(Gate * self, PyObject * const * Py_UNUSED(args),
                                 Py_ssize_t Py_UNUSED(nargs), PyObject * Py_UNUSED(kwnames)) {
            return self->executor() ? Py_NewRef(Py_True) : Py_NewRef(Py_False);
        }

        static PyObject * bind(Gate * self, PyObject * target);
        static PyObject * apply_with(Gate * self, PyObject * executor);
        static PyObject * test(Gate * self, PyObject * executor);

        // gate(executor) returns a context manager — implemented after GateContext definition
        static PyObject * call(Gate * self, PyObject ** args, size_t nargsf, PyObject * kwnames);

        static PyObject * executor_get(Gate * self, void * Py_UNUSED(closure)) {
            PyObject * exec = self->executor();
            return exec ? Py_NewRef(exec) : Py_NewRef(Py_None);
        }

        static int executor_set(Gate * self, PyObject * value, void * Py_UNUSED(closure)) {
            if (value == nullptr || value == Py_None) {
                self->set_executor(nullptr);
            } else {
                if (!PyCallable_Check(value)) {
                    PyErr_Format(PyExc_TypeError, "executor must be callable, got %S", value);
                    return -1;
                }
                self->set_executor(value);
            }
            return 0;
        }

        static PyObject * repr(Gate * self) {
            PyObject * exec = self->executor();
            if (exec) {
                return PyUnicode_FromFormat("<Gate executor=%R>", exec);
            } else {
                return PyUnicode_FromString("<Gate disabled>");
            }
        }

        static void dealloc(Gate * self) {
            // Invalidate global cache if it references this gate
            if (cache.gate == self) {
                cache = {nullptr, nullptr, {}};
            }
            Py_CLEAR(self->default_executor);
            Py_TYPE(self)->tp_free((PyObject *)self);
        }
    };

    // ========================================================================
    // BoundGate — thin wrapper returned by gate.bind(target).
    //
    // Hot path (disabled, single global Gate, same thread):
    //   1. PyThreadState_Get()          — read a global CPython maintains
    //   2. cache.tstate == tstate?      — pointer compare (almost always true)
    //   3. cache.gate == gate?          — pointer compare (always true for single gate)
    //   4. cache.executor.callable?     — NULL check
    //   5. self->target.vectorcall(...) — indirect call via cached pointer
    //
    // Hot path (active executor, PY_VECTORCALL_ARGUMENTS_OFFSET set):
    //   Steps 1-3 same, then:
    //   4. Write target into args[-1]   — reuse caller's offset slot
    //   5. cache.executor.vectorcall(...)— indirect call via cached pointer
    //   6. Restore args[-1]
    //   Zero alloca, zero memcpy.
    // ========================================================================

    struct BoundGate : public PyObject {
        Gate * gate;
        FastCall target;
        vectorcallfunc vectorcall;

        static PyObject * call(BoundGate * self, PyObject ** args, size_t nargsf, PyObject * kwnames) {
            Gate * gate = self->gate;
            PyThreadState * tstate = PyThreadState_Get();

            // Cache check: almost always a hit with a single global Gate
            if (__builtin_expect(cache.tstate != tstate || cache.gate != gate, 0)) {
                gate->load_cache(tstate);
            }

            if (cache.executor.callable == nullptr) {
                // Disabled — direct passthrough via cached vectorcall pointer
                return self->target(args, nargsf, kwnames);
            }

            // Active — call executor(target, *args, **kwargs) via cached FastCall
            Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);

            if (nargsf & PY_VECTORCALL_ARGUMENTS_OFFSET) {
                // Fast path: caller guarantees args[-1] is writable.
                // Write target into the offset slot and shift the window.
                // No alloca, no copy.
                PyObject * saved = args[-1];
                args[-1] = self->target.callable;
                // Don't propagate offset flag — we don't own args[-2]
                PyObject * result = cache.executor(args - 1, nargs + 1, kwnames);
                args[-1] = saved;
                return result;
            }

            // Fallback: build a new args array on the stack
            Py_ssize_t nkwargs = kwnames ? PyTuple_GET_SIZE(kwnames) : 0;
            Py_ssize_t total = nargs + nkwargs;

            PyObject ** new_args = (PyObject **)alloca(sizeof(PyObject *) * (total + 2));
            new_args[0] = nullptr;  // offset slot
            new_args[1] = self->target.callable;
            if (total > 0) {
                memcpy(new_args + 2, args, (size_t)total * sizeof(PyObject *));
            }

            return cache.executor(new_args + 1,
                                  (nargs + 1) | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
        }

        static PyObject * repr(BoundGate * self) {
            return PyUnicode_FromFormat("<BoundGate target=%R>", self->target.callable);
        }

        static void dealloc(BoundGate * self) {
            PyObject_GC_UnTrack(self);
            Py_XDECREF(self->gate);
            Py_XDECREF(self->target.callable);
            Py_TYPE(self)->tp_free((PyObject *)self);
        }

        static int traverse(BoundGate * self, visitproc visit, void * arg) {
            Py_VISIT(self->gate);
            Py_VISIT(self->target.callable);
            return 0;
        }

        static int clear(BoundGate * self) {
            Py_CLEAR(self->gate);
            Py_CLEAR(self->target.callable);
            return 0;
        }

        // Forward attribute access to the target for introspection
        static PyObject * getattro(BoundGate * self, PyObject * name) {
            PyObject * result = PyObject_GenericGetAttr((PyObject *)self, name);
            if (result) return result;

            PyErr_Clear();
            return PyObject_GetAttr(self->target.callable, name);
        }
    };

    // ========================================================================
    // GateContext — context manager for `with gate(executor):`.
    //
    // __enter__: saves current executor, sets new one.
    // __exit__:  restores previous executor.
    // ========================================================================

    struct GateContext : public PyObject {
        Gate * gate;
        PyObject * new_executor;   // the executor to set on __enter__ (NULL = disable)
        PyObject * old_executor;   // saved executor to restore on __exit__ (NULL = was disabled)

        static PyObject * enter(GateContext * self, PyObject * Py_UNUSED(args)) {
            PyObject * current = self->gate->executor();
            self->old_executor = current;
            Py_XINCREF(self->old_executor);

            self->gate->set_executor(self->new_executor);

            return Py_NewRef(self);
        }

        static PyObject * exit(GateContext * self, PyObject * args) {
            self->gate->set_executor(self->old_executor);
            Py_XDECREF(self->old_executor);
            self->old_executor = nullptr;

            Py_RETURN_FALSE;
        }

        static void dealloc(GateContext * self) {
            PyObject_GC_UnTrack(self);
            Py_XDECREF(self->gate);
            Py_XDECREF(self->new_executor);
            Py_XDECREF(self->old_executor);
            Py_TYPE(self)->tp_free((PyObject *)self);
        }

        static int traverse(GateContext * self, visitproc visit, void * arg) {
            Py_VISIT(self->gate);
            Py_VISIT(self->new_executor);
            Py_VISIT(self->old_executor);
            return 0;
        }

        static int clear(GateContext * self) {
            Py_CLEAR(self->gate);
            Py_CLEAR(self->new_executor);
            Py_CLEAR(self->old_executor);
            return 0;
        }
    };

    // ========================================================================
    // ApplyWith — returned by gate.apply_with(executor).
    //
    // A callable: apply_with(f, *args, **kwargs)
    //   1. Save current executor
    //   2. Set gate executor to self->executor
    //   3. Call f(*args, **kwargs)
    //   4. Restore previous executor
    //   5. Return result
    //
    // This is an inlined `with gate(executor): return f(*args, **kwargs)`
    // without context manager allocation on every call.
    // ========================================================================

    struct ApplyWith : public PyObject {
        Gate * gate;
        PyObject * executor;      // executor to temporarily activate
        vectorcallfunc vectorcall;

        static PyObject * call(ApplyWith * self, PyObject ** args, size_t nargsf, PyObject * kwnames) {
            Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);

            if (__builtin_expect(nargs < 1, 0)) {
                PyErr_SetString(PyExc_TypeError,
                    "apply_with() requires at least one argument: the function to call");
                return nullptr;
            }

            PyObject * func = args[0];
            Gate * gate = self->gate;

            // Save the current executor
            PyObject * prev = gate->executor();
            Py_XINCREF(prev);

            // Activate our executor
            gate->set_executor(self->executor);

            // Call f(*args[1:], **kwargs)
            // args[0] = func, so args+1 starts at the real arguments.
            // We can always set PY_VECTORCALL_ARGUMENTS_OFFSET because
            // (args+1)[-1] = args[0] is a valid, writable slot we own.
            PyObject * result = PyObject_Vectorcall(func, args + 1,
                (nargs - 1) | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames);

            // Restore previous executor (even on exception)
            gate->set_executor(prev);
            Py_XDECREF(prev);

            return result;
        }

        static PyObject * get(ApplyWith * self, PyObject * Py_UNUSED(args)) {
            return self->executor ? Py_NewRef(self->executor) : Py_NewRef(Py_None);
        }

        static PyObject * set(ApplyWith * self, PyObject * executor) {
            if (executor == Py_None) {
                Py_XDECREF(self->executor);
                self->executor = nullptr;
            } else {
                if (!PyCallable_Check(executor)) {
                    PyErr_Format(PyExc_TypeError,
                        "executor must be callable or None, got %S", executor);
                    return nullptr;
                }
                Py_XDECREF(self->executor);
                self->executor = Py_NewRef(executor);
            }
            Py_RETURN_NONE;
        }

        static PyObject * repr(ApplyWith * self) {
            if (self->executor) {
                return PyUnicode_FromFormat("<ApplyWith executor=%R>", self->executor);
            } else {
                return PyUnicode_FromString("<ApplyWith disabled>");
            }
        }

        static void dealloc(ApplyWith * self) {
            PyObject_GC_UnTrack(self);
            Py_XDECREF(self->gate);
            Py_XDECREF(self->executor);
            Py_TYPE(self)->tp_free((PyObject *)self);
        }

        static int traverse(ApplyWith * self, visitproc visit, void * arg) {
            Py_VISIT(self->gate);
            Py_VISIT(self->executor);
            return 0;
        }

        static int clear(ApplyWith * self) {
            Py_CLEAR(self->gate);
            Py_CLEAR(self->executor);
            return 0;
        }
    };

    // ========================================================================
    // GatePredicate — returned by gate.test(executor).
    //
    // A callable predicate that checks whether the gate's current executor
    // is a specific object (identity check). Ignores all arguments.
    // Returns Py_True if gate.executor() is executor, Py_False otherwise.
    //
    // Designed for use with functional.if_then_else to dispatch based on
    // gate state without Python overhead.
    // ========================================================================

    struct GatePredicate : public PyObject {
        Gate * gate;
        PyObject * executor;      // executor to test against (NULL = test for disabled)
        vectorcallfunc vectorcall;

        static PyObject * call(GatePredicate * self, PyObject ** args, size_t nargsf, PyObject * kwnames) {
            // Identity check against the gate's current executor
            PyObject * current = self->gate->executor();
            if (current == self->executor) {
                Py_RETURN_TRUE;
            }
            Py_RETURN_FALSE;
        }

        static PyObject * repr(GatePredicate * self) {
            if (self->executor) {
                return PyUnicode_FromFormat("<GatePredicate executor=%R>", self->executor);
            } else {
                return PyUnicode_FromString("<GatePredicate disabled>");
            }
        }

        static void dealloc(GatePredicate * self) {
            PyObject_GC_UnTrack(self);
            Py_XDECREF(self->gate);
            Py_XDECREF(self->executor);
            Py_TYPE(self)->tp_free((PyObject *)self);
        }

        static int traverse(GatePredicate * self, visitproc visit, void * arg) {
            Py_VISIT(self->gate);
            Py_VISIT(self->executor);
            return 0;
        }

        static int clear(GatePredicate * self) {
            Py_CLEAR(self->gate);
            Py_CLEAR(self->executor);
            return 0;
        }
    };

    // --- Deferred implementations (need complete types) ---

    PyObject * Gate::call(Gate * self, PyObject ** args, size_t nargsf, PyObject * kwnames) {
        Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);

        if (nargs != 1 || (kwnames && PyTuple_GET_SIZE(kwnames) > 0)) {
            PyErr_SetString(PyExc_TypeError, "Gate context manager takes exactly one argument: the executor");
            return nullptr;
        }

        PyObject * executor = args[0];
        if (executor != Py_None && !PyCallable_Check(executor)) {
            PyErr_Format(PyExc_TypeError, "executor must be callable or None, got %S", executor);
            return nullptr;
        }

        GateContext * ctx = (GateContext *)GateContext_Type.tp_alloc(&GateContext_Type, 0);
        if (!ctx) return nullptr;

        ctx->gate = (Gate *)Py_NewRef(self);
        ctx->new_executor = (executor == Py_None) ? nullptr : executor;
        Py_XINCREF(ctx->new_executor);
        ctx->old_executor = nullptr;

        return (PyObject *)ctx;
    }

    // --- Type definitions ---

    static PyMethodDef Gate_methods[] = {
        {"set", (PyCFunction)Gate::set, METH_O, "Set the executor for the current thread"},
        {"disable", (PyCFunction)Gate::disable, METH_NOARGS, "Disable the gate (set executor to NULL) for the current thread"},
        {"is_set", (PyCFunction)Gate::is_set, METH_FASTCALL | METH_KEYWORDS, "Return True if the gate has an executor set (thread-local or default), False otherwise"},
        {"bind", (PyCFunction)Gate::bind, METH_O, "Bind a target callable to this gate, returning a BoundGate wrapper"},
        {"apply_with", (PyCFunction)Gate::apply_with, METH_O, "Return an ApplyWith callable that temporarily sets the executor when called"},
        {"test", (PyCFunction)Gate::test, METH_O, "Return a predicate that tests if the gate's executor is a specific object"},
        {NULL, NULL, 0, NULL}
    };

    static PyGetSetDef Gate_getset[] = {
        {"executor", (getter)Gate::executor_get, (setter)Gate::executor_set,
         "The current thread's executor (None if disabled)"},
        {NULL, NULL, NULL, NULL}
    };

    PyTypeObject Gate_Type = {
        .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = MODULE "Gate",
        .tp_basicsize = sizeof(Gate),
        .tp_dealloc = (destructor)Gate::dealloc,
        .tp_vectorcall_offset = OFFSET_OF_MEMBER(Gate, vectorcall),
        .tp_repr = (reprfunc)Gate::repr,
        .tp_call = PyVectorcall_Call,
        .tp_str = (reprfunc)Gate::repr,
        .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_VECTORCALL,
        .tp_doc = "Gate — a thread-local function executor.\n\n"
                  "Create a Gate, bind functions to it, then set/disable the executor per thread.\n"
                  "When disabled (default), bound functions call through directly.\n"
                  "When set, all bound calls go through: executor(target, *args, **kwargs).\n"
                  "Use as a context manager: with gate(executor): ...",
        .tp_methods = Gate_methods,
        .tp_getset = Gate_getset,
        .tp_init = (initproc)Gate::init,
        .tp_new = PyType_GenericNew,
    };

    static PyMethodDef GateContext_methods[] = {
        {"__enter__", (PyCFunction)GateContext::enter, METH_NOARGS, ""},
        {"__exit__",  (PyCFunction)GateContext::exit,  METH_VARARGS, ""},
        {NULL, NULL, 0, NULL}
    };

    PyTypeObject GateContext_Type = {
        .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = MODULE "GateContext",
        .tp_basicsize = sizeof(GateContext),
        .tp_dealloc = (destructor)GateContext::dealloc,
        .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_DISALLOW_INSTANTIATION,
        .tp_doc = "Context manager for temporarily setting a Gate executor.",
        .tp_traverse = (traverseproc)GateContext::traverse,
        .tp_clear = (inquiry)GateContext::clear,
        .tp_methods = GateContext_methods,
    };

    PyTypeObject BoundGate_Type = {
        .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = MODULE "BoundGate",
        .tp_basicsize = sizeof(BoundGate),
        .tp_dealloc = (destructor)BoundGate::dealloc,
        .tp_vectorcall_offset = OFFSET_OF_MEMBER(BoundGate, vectorcall),
        .tp_repr = (reprfunc)BoundGate::repr,
        .tp_call = PyVectorcall_Call,
        .tp_getattro = (getattrofunc)BoundGate::getattro,
        .tp_flags = Py_TPFLAGS_DEFAULT |
                    Py_TPFLAGS_HAVE_GC |
                    Py_TPFLAGS_HAVE_VECTORCALL |
                    Py_TPFLAGS_DISALLOW_INSTANTIATION,
        .tp_doc = "A callable bound to a Gate. Delegates to the gate's thread-local executor.",
        .tp_traverse = (traverseproc)BoundGate::traverse,
        .tp_clear = (inquiry)BoundGate::clear,
    };

    static PyMethodDef ApplyWith_methods[] = {
        {"get", (PyCFunction)ApplyWith::get, METH_NOARGS, "Get the current executor (None if disabled)"},
        {"set", (PyCFunction)ApplyWith::set, METH_O, "Set the executor (callable or None to disable)"},
        {NULL, NULL, 0, NULL}
    };

    PyTypeObject ApplyWith_Type = {
        .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = MODULE "ApplyWith",
        .tp_basicsize = sizeof(ApplyWith),
        .tp_dealloc = (destructor)ApplyWith::dealloc,
        .tp_vectorcall_offset = OFFSET_OF_MEMBER(ApplyWith, vectorcall),
        .tp_repr = (reprfunc)ApplyWith::repr,
        .tp_call = PyVectorcall_Call,
        .tp_flags = Py_TPFLAGS_DEFAULT |
                    Py_TPFLAGS_HAVE_GC |
                    Py_TPFLAGS_HAVE_VECTORCALL |
                    Py_TPFLAGS_DISALLOW_INSTANTIATION,
        .tp_doc = "Callable that temporarily sets a Gate executor, calls f(*args, **kwargs), then restores.",
        .tp_traverse = (traverseproc)ApplyWith::traverse,
        .tp_clear = (inquiry)ApplyWith::clear,
        .tp_methods = ApplyWith_methods,
    };

    PyTypeObject GatePredicate_Type = {
        .ob_base = PyVarObject_HEAD_INIT(NULL, 0)
        .tp_name = MODULE "GatePredicate",
        .tp_basicsize = sizeof(GatePredicate),
        .tp_dealloc = (destructor)GatePredicate::dealloc,
        .tp_vectorcall_offset = OFFSET_OF_MEMBER(GatePredicate, vectorcall),
        .tp_repr = (reprfunc)GatePredicate::repr,
        .tp_call = PyVectorcall_Call,
        .tp_flags = Py_TPFLAGS_DEFAULT |
                    Py_TPFLAGS_HAVE_GC |
                    Py_TPFLAGS_HAVE_VECTORCALL |
                    Py_TPFLAGS_DISALLOW_INSTANTIATION,
        .tp_doc = "Callable predicate: returns True if the gate's executor is a specific object (identity check).",
        .tp_traverse = (traverseproc)GatePredicate::traverse,
        .tp_clear = (inquiry)GatePredicate::clear,
    };

    PyObject * Gate::test(Gate * self, PyObject * executor) {
        GatePredicate * pred = (GatePredicate *)GatePredicate_Type.tp_alloc(&GatePredicate_Type, 0);
        if (!pred) return nullptr;
        pred->gate = (Gate *)Py_NewRef(self);
        pred->executor = (executor == Py_None) ? nullptr : Py_NewRef(executor);
        pred->vectorcall = (vectorcallfunc)GatePredicate::call;
        return (PyObject *)pred;
    }

    PyObject * Gate::bind(Gate * self, PyObject * target) {
        if (!PyCallable_Check(target)) {
            PyErr_Format(PyExc_TypeError, "bind() argument must be callable, got %S", target);
            return nullptr;
        }

        BoundGate * bound = (BoundGate *)BoundGate_Type.tp_alloc(&BoundGate_Type, 0);
        if (!bound) return nullptr;

        bound->gate = (Gate *)Py_NewRef(self);
        bound->target = FastCall(Py_NewRef(target));
        bound->vectorcall = (vectorcallfunc)BoundGate::call;

        return (PyObject *)bound;
    }

    PyObject * Gate::apply_with(Gate * self, PyObject * executor) {
        if (executor != Py_None && !PyCallable_Check(executor)) {
            PyErr_Format(PyExc_TypeError,
                "apply_with() argument must be callable or None, got %S", executor);
            return nullptr;
        }

        ApplyWith * aw = (ApplyWith *)ApplyWith_Type.tp_alloc(&ApplyWith_Type, 0);
        if (!aw) return nullptr;

        aw->gate = (Gate *)Py_NewRef(self);
        aw->executor = (executor == Py_None) ? nullptr : Py_NewRef(executor);
        aw->vectorcall = (vectorcallfunc)ApplyWith::call;

        return (PyObject *)aw;
    }
}
