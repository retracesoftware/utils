#include "utils.h"
#include <exception>
#include <structmember.h>
#include "unordered_dense.h"
using namespace ankerl::unordered_dense;

namespace retracesoftware {

    static map<PyTypeObject *, allocfunc> allocfuncs;
    static map<PyTypeObject *, destructor> deallocfuncs;
    static map<PyObject *, PyObject *> dealloc_callbacks;

    static PyObject * key() {
        static PyObject * name = nullptr;
        if (!name) name = PyUnicode_InternFromString("__retrace_on_alloc__");
        return name;
    }

    static PyObject * find_callback(PyTypeObject * cls) {
        while (cls) {
            PyObject * callback = PyDict_GetItem(cls->tp_dict, key());
            if (callback) return callback;
            cls = cls->tp_base;
        }
        return nullptr;
    }

    static bool call_callback(PyObject * allocated) {
        PyObject * callback = find_callback(Py_TYPE(allocated));
        if (!callback) return true;

        PyObject * result = PyObject_CallOneArg(callback, allocated);
        if (!result) return false;

        if (result == Py_None) {
            Py_DECREF(result);
            return true;
        }

        if (PyCallable_Check(result)) {
            dealloc_callbacks[allocated] = result;  // steals the reference
            return true;
        }

        Py_DECREF(result);
        PyErr_Format(PyExc_TypeError,
            "__retrace_on_alloc__ callback must return None or a callable, "
            "got %.200s", Py_TYPE(result)->tp_name);
        return false;
    }

    // generic_dealloc — tp_dealloc trampoline for set_on_alloc
    //
    // Called when an object whose type (or ancestor type) was patched by
    // set_on_alloc is about to be freed.  Two responsibilities:
    //
    //   1. Run and remove any per-object dealloc callback registered by
    //      the alloc-time on_alloc hook (stored in dealloc_callbacks).
    //
    //   2. Forward to the original tp_dealloc that was saved before we
    //      replaced the slot.
    //
    // The tricky part is step 2.  CPython's heap-type dealloc chain works
    // like this for a hierarchy  Base → Sub → SubSub:
    //
    //   SubSub->tp_dealloc == subtype_dealloc   (installed by type_new)
    //   Sub->tp_dealloc    == subtype_dealloc
    //   Base->tp_dealloc   == base_specific_dealloc  (e.g. sock_dealloc)
    //
    // subtype_dealloc walks tp_base until it finds a tp_dealloc that
    // differs from itself, then calls it.  It finishes with a single
    // Py_DECREF(Py_TYPE(obj)) for the heap type.
    //
    // When we insert generic_dealloc into this chain we must avoid two
    // pitfalls:
    //
    //   A) Re-entry confusion:  subtype_dealloc calls base->tp_dealloc
    //      (generic_dealloc).  Inside generic_dealloc, Py_TYPE(obj) is
    //      still the leaf subtype, NOT the base.  Using Py_TYPE(obj) to
    //      look up the original dealloc would find the *subtype's* saved
    //      dealloc (subtype_dealloc), call it, and recurse forever.
    //
    //   B) Double Py_DECREF(type):  if the original dealloc we stored for
    //      a heap type is subtype_dealloc and we call it from inside an
    //      outer subtype_dealloc, the inner call does Py_DECREF(type),
    //      then the outer also does Py_DECREF(type) — double-decref
    //      drives the type's refcount negative.
    //
    // We solve both by detecting re-entry (Py_TYPE(obj)->tp_dealloc was
    // temporarily restored to the original by an outer trampoline, so it
    // is != generic_dealloc) and then:
    //
    //   - Walking tp_base to find the type whose slot still points to
    //     generic_dealloc (solves A).
    //
    //   - For heap types on re-entry, skipping the stored subtype_dealloc
    //     and continuing directly to the next base dealloc (solves B).
    //
    static void generic_dealloc(PyObject * obj) {
        auto it = dealloc_callbacks.find(obj);
        if (it != dealloc_callbacks.end()) {
            PyObject * cb = it->second;
            dealloc_callbacks.erase(it);

            Py_SET_REFCNT(obj, 1);
            PyObject * r = PyObject_CallNoArgs(cb);
            Py_XDECREF(r);
            Py_DECREF(cb);
            if (!r) PyErr_Clear();

            if (Py_REFCNT(obj) > 1) {
                Py_SET_REFCNT(obj, Py_REFCNT(obj) - 1);
                return;
            }
            Py_SET_REFCNT(obj, 0);
        }

        PyTypeObject * type = Py_TYPE(obj);
        bool called_from_chain = (type->tp_dealloc != generic_dealloc);

        // On re-entry Py_TYPE(obj) is the leaf subtype whose slot was
        // temporarily restored.  Walk tp_base to find our level.
        if (called_from_chain) {
            type = type->tp_base;
            while (type && type->tp_dealloc != generic_dealloc)
                type = type->tp_base;
            if (!type) return;
        }

        auto dit = deallocfuncs.find(type);
        if (dit == deallocfuncs.end()) return;

        destructor original = dit->second;

        if (called_from_chain && (type->tp_flags & Py_TPFLAGS_HEAPTYPE)) {
            // Heap type whose original is subtype_dealloc.  An outer
            // subtype_dealloc already owns the final Py_DECREF(type).
            // Re-invoking subtype_dealloc would double-decref the type
            // and corrupt its refcount.  Skip it and jump to the next
            // non-subtype_dealloc base (same as what subtype_dealloc
            // itself would find).
            type->tp_dealloc = original;
            PyTypeObject * base = type->tp_base;
            while (base && base->tp_dealloc == original)
                base = base->tp_base;
            if (base)
                base->tp_dealloc(obj);
            type->tp_dealloc = generic_dealloc;
        } else {
            // C base type or first-entry: normal trampoline.
            type->tp_dealloc = original;
            original(obj);
            type->tp_dealloc = generic_dealloc;
        }
    }

    static PyObject * generic_alloc(PyTypeObject *type, Py_ssize_t nitems) {

        auto it = allocfuncs.find(type);
        if (it == allocfuncs.end()) {
            PyErr_Format(PyExc_RuntimeError, "Original tp_alloc mapping for type: %S not found",
                            type);
            return nullptr;
        }
        PyObject * obj = it->second(type, nitems);

        if (obj) {
            if (!call_callback(obj)) {
                Py_DECREF(obj);
                return nullptr;
            }
        }
        return obj;
    }

    static PyObject * PyType_GenericAlloc_Wrapper(PyTypeObject *type, Py_ssize_t nitems) {

        PyObject * obj = PyType_GenericAlloc(type, nitems);

        if (obj) {
            if (!call_callback(obj)) {
                Py_DECREF(obj);
                return nullptr;
            }
        }
        return obj;
    }

    static bool is_alloc_patched(allocfunc func) {
        return func == PyType_GenericAlloc_Wrapper || func == generic_alloc;
    }

    static void patch_alloc(PyTypeObject * cls) {
        if (cls->tp_alloc == PyType_GenericAlloc) {
            cls->tp_alloc = PyType_GenericAlloc_Wrapper;
        } else {
            allocfuncs[cls] = cls->tp_alloc;
            cls->tp_alloc = generic_alloc;
        }
    }

    static void patch_dealloc(PyTypeObject * cls) {
        if (cls->tp_dealloc && cls->tp_dealloc != generic_dealloc) {
            deallocfuncs[cls] = cls->tp_dealloc;
            cls->tp_dealloc = generic_dealloc;
        }
    }

    bool set_on_alloc(PyTypeObject *type, PyObject * callback) {
        if (!is_alloc_patched(type->tp_alloc)) {
            patch_alloc(type);
        }

        // Only patch tp_dealloc when no ancestor already has generic_dealloc.
        //
        // CPython's subtype_dealloc walks tp_base until it finds a
        // tp_dealloc that differs from subtype_dealloc, then calls it.
        // If an ancestor already has generic_dealloc, subtype_dealloc
        // will reach it naturally.  Patching the subtype as well would
        // insert a second generic_dealloc in the chain, causing the
        // re-entry → double Py_DECREF(type) described in generic_dealloc.
        bool ancestor_patched = false;
        for (PyTypeObject * b = type->tp_base; b; b = b->tp_base) {
            if (b->tp_dealloc == generic_dealloc) {
                ancestor_patched = true;
                break;
            }
        }
        if (!ancestor_patched)
            patch_dealloc(type);

        return PyDict_SetItem(type->tp_dict, key(), callback) == 0 ? true : false;
    }
}