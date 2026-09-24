#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

/* The wrapper may live in PSRAM; the C11 atomic value must live in DIRAM. */
#define H2_ATOMIC_C11_ALLOC(size) \
    heap_caps_malloc((size), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define H2_ATOMIC_C11_FREE(pointer) heap_caps_free(pointer)
/* The provider uses a stronger fixed order for every operation. */
#define H2_ATOMIC_C11_ORDER(order) ((void)(order), memory_order_seq_cst)
/* Keep the inline flag byte usable in PSRAM. A short ESP critical section
 * prevents a higher-priority task from preempting the lock holder on the same
 * core; the generic C11 spinlock can otherwise starve that holder forever. */
static DRAM_ATTR portMUX_TYPE s_flag_mux = portMUX_INITIALIZER_UNLOCKED;
#define H2_ATOMIC_C11_FLAG_LOCK() portENTER_CRITICAL_SAFE(&s_flag_mux)
#define H2_ATOMIC_C11_FLAG_UNLOCK() portEXIT_CRITICAL_SAFE(&s_flag_mux)
#include "h2_atomic_c11_impl.h"
