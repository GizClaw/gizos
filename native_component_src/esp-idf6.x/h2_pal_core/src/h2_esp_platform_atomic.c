#include "h2_esp_platform_core.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include <stdint.h>
#include <stdlib.h>

/* S32C1I is not cross-core atomic in PSRAM on ESP32-S3. Small shared atomics
 * therefore live in a DRAM slot pool. Each slot is 8-byte aligned. */
#define H2_ESP_ATOMIC_POOL_SLOTS 64u
#if CONFIG_SPIRAM
static DRAM_ATTR _Alignas(8) uint8_t s_slots[H2_ESP_ATOMIC_POOL_SLOTS][8];
static DRAM_ATTR uint64_t s_used;
static DRAM_ATTR portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#endif

static h2_pal_result_t h2_esp_atomic_alloc(void *user, size_t size,
                                             size_t alignment, void **out) {
    (void)user;
    if (out == NULL || size == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0) return H2_PAL_ERR_INVALID_ARG;
    *out = NULL;
    if (alignment > 8) return H2_PAL_ERR_UNSUPPORTED;
#if CONFIG_SPIRAM
    if (size <= 8) {
        portENTER_CRITICAL(&s_lock);
        for (unsigned i = 0; i < H2_ESP_ATOMIC_POOL_SLOTS; ++i) {
            uint64_t bit = UINT64_C(1) << i;
            if ((s_used & bit) == 0) {
                s_used |= bit;
                *out = s_slots[i];
                break;
            }
        }
        portEXIT_CRITICAL(&s_lock);
        if (*out != NULL) return H2_PAL_OK;
    }
    *out = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    *out = malloc(size);
#endif
    return *out != NULL ? H2_PAL_OK : H2_PAL_ERR_NO_MEMORY;
}

static void h2_esp_atomic_free(void *user, void *value) {
    (void)user;
    if (value == NULL) return;
#if CONFIG_SPIRAM
    uintptr_t start = (uintptr_t)&s_slots[0][0];
    uintptr_t address = (uintptr_t)value;
    if (address >= start && address < start + sizeof(s_slots)) {
        size_t offset = (size_t)(address - start);
        if (offset % 8 == 0) {
            portENTER_CRITICAL(&s_lock);
            s_used &= ~(UINT64_C(1) << (offset / 8));
            portEXIT_CRITICAL(&s_lock);
        }
        return;
    }
    heap_caps_free(value);
#else
    free(value);
#endif
}

static const h2_pal_atomic_vtable_t s_vtable = {
    .alloc = h2_esp_atomic_alloc, .free = h2_esp_atomic_free,
};
static const h2_pal_atomic_api_t s_api = { .user = NULL, .vtable = &s_vtable };

const h2_pal_atomic_api_t *h2_esp_platform_atomic_api(void) {
    return &s_api;
}
