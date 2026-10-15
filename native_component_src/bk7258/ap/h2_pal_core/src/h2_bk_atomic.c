#include <os/os.h>
#include <os/mem.h>
#include "FreeRTOS.h"
#include "portable.h"

/* BK's critical section masks local interrupts and takes the SDK's SRAM
 * spinlock across both AP cores. It provides a stronger order than requested.
 * The public wrapper may reside in PSRAM; dynamic storage stays in SRAM. */
typedef uint32_t h2_atomic_platform_lock_state_t;
static h2_atomic_platform_lock_state_t h2_atomic_platform_lock(void) {
    return rtos_enter_critical();
}
static void h2_atomic_platform_unlock(h2_atomic_platform_lock_state_t state) {
    rtos_exit_critical(state);
}
#if CONFIG_MALLOC_STATIS || CONFIG_MEM_DEBUG
#define H2_ATOMIC_PLATFORM_ALLOC(size) bk_wrap_sram_malloc_cm(__func__, __LINE__, (size), 0)
#else
#define H2_ATOMIC_PLATFORM_ALLOC(size) bk_wrap_sram_malloc(size)
#endif
#define H2_ATOMIC_PLATFORM_FREE(pointer) os_free(pointer)
#include "h2_atomic_locked_impl.h"
