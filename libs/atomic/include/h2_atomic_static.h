#ifndef H2_ATOMIC_STATIC_H
#define H2_ATOMIC_STATIC_H

#ifdef __cplusplus
#error "C++ callers use opaque h2_atomic wrappers with explicit init/destroy"
#endif

#include "h2_atomic.h"

#include <stdatomic.h>

/* C providers and file-static definitions share these backing layouts.
 * Ordinary h2_atomic.h consumers do not need to parse C11 atomic syntax. */
#define H2_ATOMIC_STORAGE_TYPE(name, type) \
    struct h2_atomic_##name##_storage { _Atomic(type) value; bool heap_owned; }
H2_ATOMIC_STORAGE_TYPE(int, int);
H2_ATOMIC_STORAGE_TYPE(uint, unsigned int);
H2_ATOMIC_STORAGE_TYPE(u8, uint8_t);
H2_ATOMIC_STORAGE_TYPE(u16, uint16_t);
H2_ATOMIC_STORAGE_TYPE(u32, uint32_t);
H2_ATOMIC_STORAGE_TYPE(bool, bool);
H2_ATOMIC_STORAGE_TYPE(size, size_t);
H2_ATOMIC_STORAGE_TYPE(ptr, void *);
H2_ATOMIC_STORAGE_TYPE(flag, uint32_t);
#undef H2_ATOMIC_STORAGE_TYPE

/* One ordinary file-static backing per value. Platform linker placement is
 * the target's normal static-data contract; this macro adds no attributes or
 * platform selection. A static wrapper is ready without runtime allocation.
 * Calling init on it returns INVALID_STATE. Destroy is a no-op: static storage
 * is never freed and the wrapper stays usable for the process lifetime. */
#define H2_ATOMIC_DEFINE_STATIC(kind, name, initial) \
    static h2_atomic_##kind##_storage_t name##_h2_storage = { \
        (initial), false \
    }; \
    static h2_atomic_##kind##_t name = { &name##_h2_storage }

#endif
