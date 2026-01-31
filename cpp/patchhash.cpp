#include "utils.h"
#include "unordered_dense.h"

using namespace ankerl::unordered_dense;

#include <stdint.h>

/* Inverse of spread64 */
static inline uint64_t unspread64(uint64_t x) {
    // undo x ^= x >> 31
    x ^= x >> 31;
    x ^= x >> 62;

    // undo multiply by 0x94d049bb133111eb (mod 2^64)
    x *= 0x319642b2d24d8ec3ULL; // modular inverse

    // undo x ^= x >> 27
    x ^= x >> 27;
    x ^= x >> 54;

    // undo multiply by 0xbf58476d1ce4e5b9 (mod 2^64)
    x *= 0x96de1b173f119089ULL; // modular inverse

    // undo x ^= x >> 30
    x ^= x >> 30;
    x ^= x >> 60;

    return x;
}

static inline uint64_t spread64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

// static Py_hash_t patched_hash(PyObject * self) {
//     auto it = hashes.find(self);

//     printf("patched_hash count: %i\n", foo++);

//     if (it == hashes.end()) {
//         printf("new hash count: %i\n", foo1++);

//         Py_hash_t hash = enabled ? spread64(counter++) : _Py_HashPointer(self);
        
//         hashes[self] = hash;

//         return hash;
//     } else {
//         return it->second;
//     }
// }

namespace retracesoftware {

    struct Hasher {
        FastCall hashfunc;
        destructor dealloc;
    
        // Copy Constructor
        Hasher(const Hasher& other) : hashfunc(other.hashfunc), dealloc(other.dealloc) {}

        // Copy Assignment Operator
        Hasher& operator=(const Hasher& other) {
            if (this != &other) {
                hashfunc = other.hashfunc;
                dealloc = other.dealloc;
            }
            return *this;
        }

        Hasher(PyTypeObject * cls, PyObject * hasher) :
            hashfunc(FastCall(hasher)), dealloc(cls->tp_dealloc) {}

        Py_hash_t hash(PyObject * self) {
            PyObject * result = hashfunc(self);

            if (!result) return -1;

            if (result == Py_None) {
                Py_DECREF(result);
                return _Py_HashPointer(self);
            }

            if (PyLong_CheckExact(result)) {
                Py_hash_t hash = PyLong_AsLongLong(result);
                Py_DECREF(result);
                if (hash == -1) hash = 1;
                return hash;
            }

            if (Py_TYPE(result) == &Counter_Type) {
                Py_hash_t hash = (Py_hash_t)spread64(Counter_Next(result));
                Py_DECREF(result);
                if (hash == -1) hash = 1;
                return hash;
            }

            PyErr_Format(PyExc_TypeError, "hash function: %S returned object: %S of unexpected type: %S", 
                            hashfunc.callable,
                            result, 
                            Py_TYPE(result));

            return -1;
        }  
    };

    static map<PyObject *, Py_hash_t> hashes;
    static map<PyTypeObject *, Hasher> hashers;
           
    static Hasher& find_hasher(PyTypeObject * cls) {
        auto it = hashers.find(cls);
        if (it != hashers.end()) return it->second;
        else return find_hasher(cls->tp_base);
    }

    static Py_hash_t hashfunc(PyObject * self) {
        auto it = hashes.find(self);
        if (it != hashes.end()) return it->second;
        else {
            Py_hash_t hash = find_hasher(Py_TYPE(self)).hash(self);

            if (hash != -1) {
                hashes[self] = hash;
            }
            return hash;
        }
    };

    static void patched_dealloc(PyObject * self) {
        hashes.erase(self);
        find_hasher(Py_TYPE(self)).dealloc(self);
    }

    void patch_hash(PyTypeObject * cls, PyObject * hashfunc) {

        assert (!hashers.contains(cls));

        Py_INCREF(cls);
        Py_INCREF(hashfunc);

        hashers.emplace(cls, Hasher(cls, hashfunc));

        cls->tp_dealloc = patched_dealloc;
        cls->tp_hash = retracesoftware::hashfunc;
    }
}
