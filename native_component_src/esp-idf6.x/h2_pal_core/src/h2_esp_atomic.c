#include "esp_heap_caps.h"

/* The wrapper may live in PSRAM; the C11 atomic value must live in DIRAM. */
#define H2_ATOMIC_C11_ALLOC(size) \
    heap_caps_malloc((size), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define H2_ATOMIC_C11_FREE(pointer) heap_caps_free(pointer)
#include "h2_atomic_c11_impl.h"
