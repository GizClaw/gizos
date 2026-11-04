#include "h2_esp_platform_core.h"
#include "esp_heap_caps.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* An SDK heap with rounded usable sizes and allocation failure, so the PAL
 * must preserve data, alignment and ownership across realloc transitions. */
static struct {
  void *raw, *ptr;
  size_t usable;
} blocks[32];
static int fail_next;
static unsigned last_caps;
void* heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps) {
  if (fail_next) {
    fail_next = 0;
    return NULL;
  }
  assert(alignment >= _Alignof(max_align_t));
  last_caps = caps;
  size_t usable = (size + 15u) & ~(size_t)15u;
  if (usable == 0) usable = 16u;
  void* raw = malloc(usable + alignment);
  assert(raw != NULL);
  void* ptr =
      (void*)(((uintptr_t)raw + alignment - 1u) & ~(uintptr_t)(alignment - 1u));
  for (unsigned i = 0; i < 32u; ++i) {
    if (blocks[i].ptr != NULL) continue;
    blocks[i].raw = raw;
    blocks[i].ptr = ptr;
    blocks[i].usable = usable;
    return ptr;
  }
  abort();
}
size_t heap_caps_get_allocated_size(void* ptr) {
  for (unsigned i = 0; i < 32u; ++i)
    if (blocks[i].ptr == ptr) return blocks[i].usable;
  abort();
}
void heap_caps_free(void* ptr) {
  if (ptr == NULL) return;
  for (unsigned i = 0; i < 32u; ++i) {
    if (blocks[i].ptr != ptr) continue;
    free(blocks[i].raw);
    memset(&blocks[i], 0, sizeof(blocks[i]));
    return;
  }
  abort();
}
static unsigned expected_stats_caps, stats_queries;
static h2_pal_mem_stats_t observed_stats = {
    .total_bytes = 65536u,
    .free_bytes = 32768u,
    .minimum_free_bytes = 8192u,
    .largest_free_block_bytes = 16384u,
};
size_t heap_caps_get_total_size(unsigned caps) {
  assert(caps == expected_stats_caps && stats_queries++ == 0u);
  return observed_stats.total_bytes;
}
size_t heap_caps_get_free_size(unsigned caps) {
  assert(caps == expected_stats_caps && stats_queries++ == 1u);
  return observed_stats.free_bytes;
}
size_t heap_caps_get_minimum_free_size(unsigned caps) {
  assert(caps == expected_stats_caps && stats_queries++ == 2u);
  return observed_stats.minimum_free_bytes;
}
size_t heap_caps_get_largest_free_block(unsigned caps) {
  assert(caps == expected_stats_caps && stats_queries++ == 3u);
  return observed_stats.largest_free_block_bytes;
}

static h2_esp_platform_resource_stats_t stats(void) {
  h2_esp_platform_resource_stats_t value;
  assert(h2_esp_platform_get_resource_stats(&value) == H2_PAL_OK);
  return value;
}
static void test_whole_region_observations(void) {
  const h2_pal_mem_api_t* apis[] = {
      h2_esp_platform_default_allocator(),
      h2_esp_platform_psram_allocator(),
      h2_esp_platform_internal_allocator(),
      h2_esp_platform_dma_allocator(),
  };
  const h2_pal_mem_region_t regions[] = {
      H2_PAL_MEM_REGION_INTERNAL,
      H2_PAL_MEM_REGION_INTERNAL_EXECUTABLE,
      H2_PAL_MEM_REGION_EXTERNAL,
  };
  const unsigned masks[] = {
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
      MALLOC_CAP_INTERNAL | MALLOC_CAP_IRAM_8BIT,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
  };
  const h2_pal_mem_stats_t empty = {0};
  for (size_t i = 0u; i < sizeof(apis) / sizeof(apis[0]); ++i) {
    const h2_pal_mem_api_t* api = apis[i];
    void* block = h2_pal_mem_alloc(api, 32u);
    assert(block != NULL && stats().allocation_bytes == 32u);
    for (size_t j = 0u; j < sizeof(regions) / sizeof(regions[0]); ++j) {
      h2_pal_mem_stats_t value;
      memset(&value, 0xa5, sizeof(value));
      expected_stats_caps = masks[j];
      stats_queries = 0u;
      assert(h2_pal_mem_get_stats(api, regions[j], &value) == H2_PAL_OK);
      assert(stats_queries == 4u);
      assert(memcmp(&value, &observed_stats, sizeof(value)) == 0);
      assert(stats().allocations == 1u && stats().allocation_bytes == 32u);
    }
    h2_pal_mem_stats_t value;
    stats_queries = 0u;
    assert(api->vtable->get_stats(api->user, (h2_pal_mem_region_t)99, &value) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->get_stats(api->user, regions[0], NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(stats_queries == 0u);
    h2_pal_mem_free(api, block);
  }
  const h2_pal_mem_api_t* api = apis[0];
  h2_pal_mem_stats_t value;
  /* Known absent and exhausted regions both have valid observations. */
  observed_stats = empty;
  expected_stats_caps = masks[2];
  stats_queries = 0u;
  memset(&value, 0xa5, sizeof(value));
  assert(api->vtable->get_stats(api->user, regions[2], &value) == H2_PAL_OK);
  assert(stats_queries == 1u && memcmp(&value, &empty, sizeof(value)) == 0);
  observed_stats.total_bytes = 65536u;
  expected_stats_caps = masks[0];
  stats_queries = 0u;
  assert(h2_pal_mem_get_stats(api, regions[0], &value) == H2_PAL_OK);
  assert(stats_queries == 4u);
  assert(value.total_bytes == 65536u && value.free_bytes == 0u &&
         value.minimum_free_bytes == 0u &&
         value.largest_free_block_bytes == 0u);
}

static void test_optional_external_allocator(void) {
  expected_stats_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  stats_queries = 0u;
  observed_stats.total_bytes = 0u;
  assert(h2_esp_platform_external_allocator() == NULL && stats_queries == 1u);
  stats_queries = 0u;
  observed_stats.total_bytes = 65536u;
  assert(h2_esp_platform_external_allocator() ==
         h2_esp_platform_psram_allocator());
  assert(stats_queries == 1u);
}

int main(void) {
  test_optional_external_allocator();
  h2_pal_mem_api_t* apis[] = {
      h2_esp_platform_default_allocator(),
      h2_esp_platform_psram_allocator(),
      h2_esp_platform_internal_allocator(),
      h2_esp_platform_dma_allocator(),
  };
  const unsigned caps[] = {MALLOC_CAP_8BIT, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM,
                           MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL,
                           MALLOC_CAP_8BIT | MALLOC_CAP_DMA};
  assert(h2_esp_platform_get_resource_stats(NULL) == H2_PAL_ERR_INVALID_ARG);
  for (unsigned i = 0; i < 4u; ++i) {
    h2_pal_mem_api_t* api = apis[i];
    for (size_t size = 1u; size < 200u; size += 17u) {
      unsigned char* p = h2_pal_mem_alloc(api, size);
      assert(p != NULL && (uintptr_t)p % _Alignof(max_align_t) == 0);
      assert(last_caps == caps[i]);
      memset(p, 0x5a, size);
      fail_next = 1;
      assert(h2_pal_mem_realloc(api, p, size + 31u) == NULL);
      assert(stats().allocations == 1u);
      assert(stats().allocation_bytes == heap_caps_get_allocated_size(p));
      for (size_t k = 0; k < size; ++k) assert(p[k] == 0x5a);
      p = h2_pal_mem_realloc(api, p, size + 31u);
      assert(p != NULL && (uintptr_t)p % _Alignof(max_align_t) == 0);
      for (size_t k = 0; k < size; ++k) assert(p[k] == 0x5a);
      p = h2_pal_mem_realloc(api, p, 1u);
      assert(p != NULL && p[0] == 0x5a);
      assert((uintptr_t)p % _Alignof(max_align_t) == 0);
      assert(stats().allocations == 1u && stats().allocation_bytes == 16u);
      assert(h2_pal_mem_realloc(api, p, 0u) == NULL);
      assert(stats().allocations == 0u && stats().allocation_bytes == 0u);
    }
    fail_next = 1;
    assert(h2_pal_mem_alloc(api, 64u) == NULL);
    void* p = h2_pal_mem_realloc(api, NULL, 5u);
    assert(p != NULL);
    h2_pal_mem_free(api, p);
    h2_pal_mem_free(api, NULL);
    assert(stats().allocations == 0u && stats().allocation_bytes == 0u);
  }
  test_whole_region_observations();
  return 0;
}
