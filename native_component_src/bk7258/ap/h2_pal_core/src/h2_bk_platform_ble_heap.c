#include <common/sys_config.h>

#if CONFIG_BT
#include <os/mem.h>

void *__wrap__malloc_wrapper(size_t size) {
    return psram_malloc(size);
}

void __wrap__free_wrapper(void *memory) {
    psram_free(memory);
}
#endif
