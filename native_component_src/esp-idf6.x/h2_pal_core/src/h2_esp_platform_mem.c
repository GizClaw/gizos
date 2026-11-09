#include "h2_esp_platform_core.h"
#include "h2_esp_resource_stats_internal.h"

#include "esp_heap_caps.h"

#include <string.h>

typedef struct h2_esp_heap_context {
  uint32_t caps;
} h2_esp_heap_context_t;

static void* esp_platform_alloc(void* user, size_t len) {
  const h2_esp_heap_context_t* ctx = (const h2_esp_heap_context_t*)user;
  void* ptr = heap_caps_aligned_alloc(_Alignof(max_align_t), len, ctx->caps);
  if (ptr != NULL) h2_esp_memory_acquire(heap_caps_get_allocated_size(ptr));
  return ptr;
}

static void* esp_platform_realloc(void* user, void* ptr, size_t len) {
  const h2_esp_heap_context_t* ctx = (const h2_esp_heap_context_t*)user;
  /* IDF realloc may move an aligned block to a merely 4-byte-aligned one.
   * Preserve the PAL malloc contract, including the old block on failure. */
  if (ptr == NULL) return esp_platform_alloc(user, len);
  size_t before = heap_caps_get_allocated_size(ptr);
  if (len == 0u) {
    h2_esp_memory_release(before);
    heap_caps_free(ptr);
    return NULL;
  }
  void* next = heap_caps_aligned_alloc(_Alignof(max_align_t), len, ctx->caps);
  if (next != NULL) {
    memcpy(next, ptr, before < len ? before : len);
    heap_caps_free(ptr);
    h2_esp_memory_release(before);
    h2_esp_memory_acquire(heap_caps_get_allocated_size(next));
  }
  return next;
}

static void esp_platform_free(void* user, void* ptr) {
  (void)user;
  if (ptr != NULL) h2_esp_memory_release(heap_caps_get_allocated_size(ptr));
  heap_caps_free(ptr);
}

static h2_pal_result_t esp_platform_get_stats(void* user,
                                              h2_pal_mem_region_t region,
                                              h2_pal_mem_stats_t* out_stats) {
  (void)user;
  if (out_stats == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_stats, 0, sizeof(*out_stats));
  uint32_t caps;
  switch (region) {
    case H2_PAL_MEM_REGION_INTERNAL:
      caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
      break;
    case H2_PAL_MEM_REGION_INTERNAL_EXECUTABLE:
      caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT;
      break;
    case H2_PAL_MEM_REGION_EXTERNAL:
      caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
      break;
    default:
      return H2_PAL_ERR_INVALID_ARG;
  }
  const size_t total = heap_caps_get_total_size(caps);
  /* An empty capability region is a known zero-sized heap, not an unknown
   * observation. Keep the other regions usable on devices without it. */
  if (total == 0u) return H2_PAL_OK;
  *out_stats = (h2_pal_mem_stats_t){
      .total_bytes = total,
      .free_bytes = heap_caps_get_free_size(caps),
      .minimum_free_bytes = heap_caps_get_minimum_free_size(caps),
      .largest_free_block_bytes = heap_caps_get_largest_free_block(caps),
  };
  return H2_PAL_OK;
}

static h2_pal_mem_api_t* esp_platform_allocator(uint32_t caps) {
  static const h2_pal_mem_vtable_t vtable = {
      .alloc = esp_platform_alloc,
      .realloc = esp_platform_realloc,
      .free = esp_platform_free,
      .get_stats = esp_platform_get_stats,
  };
  static h2_esp_heap_context_t contexts[4];
  static h2_pal_mem_api_t allocators[4];
  static int initialized;

  if (!initialized) {
    contexts[0].caps = MALLOC_CAP_8BIT;
    contexts[1].caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    contexts[2].caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    contexts[3].caps = MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    for (size_t i = 0u; i < 4u; ++i) {
      allocators[i].user = &contexts[i];
      allocators[i].vtable = &vtable;
    }
    initialized = 1;
  }

  for (size_t i = 0u; i < 4u; ++i) {
    if (contexts[i].caps == caps) {
      return &allocators[i];
    }
  }
  return &allocators[0];
}

h2_pal_mem_api_t* h2_esp_platform_default_allocator(void) {
  return esp_platform_allocator(MALLOC_CAP_8BIT);
}

h2_pal_mem_api_t* h2_esp_platform_psram_allocator(void) {
  return esp_platform_allocator(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

const h2_pal_mem_api_t* h2_esp_platform_external_allocator(void) {
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == 0u)
    return NULL;
  return h2_esp_platform_psram_allocator();
}

h2_pal_mem_api_t* h2_esp_platform_internal_allocator(void) {
  return esp_platform_allocator(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

h2_pal_mem_api_t* h2_esp_platform_dma_allocator(void) {
  return esp_platform_allocator(MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}
