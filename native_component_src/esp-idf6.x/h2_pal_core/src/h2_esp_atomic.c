#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

static portMUX_TYPE s_h2_atomic_lock = portMUX_INITIALIZER_UNLOCKED;
typedef int h2_atomic_platform_lock_state_t;
static h2_atomic_platform_lock_state_t h2_atomic_platform_lock(void) {
    portENTER_CRITICAL(&s_h2_atomic_lock);
    return 0;
}
static void h2_atomic_platform_unlock(h2_atomic_platform_lock_state_t state) {
    (void)state;
    portEXIT_CRITICAL(&s_h2_atomic_lock);
}

#define H2_ATOMIC_PLATFORM_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define H2_ATOMIC_PLATFORM_FREE(pointer) heap_caps_free(pointer)
#include "h2_atomic_locked_impl.h"
