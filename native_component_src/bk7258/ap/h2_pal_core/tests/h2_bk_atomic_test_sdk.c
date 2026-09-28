#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
static pthread_mutex_t critical = PTHREAD_MUTEX_INITIALIZER;
uint32_t rtos_enter_critical(void) {
    assert(pthread_mutex_lock(&critical) == 0);
    return 0x55u;
}
void rtos_exit_critical(uint32_t state) {
    assert(state == 0x55u);
    assert(pthread_mutex_unlock(&critical) == 0);
}
void *bk_wrap_sram_malloc(size_t size) { return malloc(size); }
void os_free(void *ptr) { free(ptr); }
