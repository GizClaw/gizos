#include "h2_bk_platform_core.h"
#include "h2_bk_resource_stats_internal.h"
#include "components/system.h"
#include "os/mem.h"
#include "os/os.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t bluetooth_mac[6] = {0x30, 0xed, 0xa0, 0xae, 0x0f, 0x86};
static int mac_result;
static unsigned mac_calls;
static size_t allocations, allocation_bytes;

static size_t heap_total[2] = {65536u, 1048576u};
static size_t heap_free[2] = {49152u, 786432u};
static size_t heap_minimum[2] = {32768u, 524288u};
static unsigned heap_calls[2];
size_t rtos_get_total_heap_size(void) {
  ++heap_calls[0];
  return heap_total[0];
}
size_t rtos_get_free_heap_size(void) {
  ++heap_calls[0];
  return heap_free[0];
}
size_t rtos_get_minimum_free_heap_size(void) {
  ++heap_calls[0];
  return heap_minimum[0];
}
size_t rtos_get_psram_total_heap_size(void) {
  ++heap_calls[1];
  return heap_total[1];
}
size_t rtos_get_psram_free_heap_size(void) {
  ++heap_calls[1];
  return heap_free[1];
}
size_t rtos_get_psram_minimum_free_heap_size(void) {
  ++heap_calls[1];
  return heap_minimum[1];
}

typedef union block_header {
#if defined(_MSC_VER) && !defined(__clang__)
  /* MSVC's C headers omit max_align_t; cover its scalar alignments. */
  long double alignment;
  long long integer_alignment;
  void* pointer_alignment;
#else
  max_align_t alignment;
#endif
  struct {
    size_t size;
    unsigned heap;
  } allocation;
} block_header_t;

int bk_get_mac(uint8_t* mac, mac_type_t type) {
  assert(type == MAC_TYPE_BLUETOOTH);
  ++mac_calls;
  memcpy(mac, bluetooth_mac, sizeof(bluetooth_mac));
  return mac_result;
}

static void observe_allocation(unsigned heap, size_t before, size_t after) {
  assert(heap_free[heap] + before >= after);
  heap_free[heap] += before;
  heap_free[heap] -= after;
  if (heap_free[heap] < heap_minimum[heap])
    heap_minimum[heap] = heap_free[heap];
}
static void* sdk_realloc(void* ptr, size_t size, unsigned heap) {
  block_header_t* block = ptr == NULL ? NULL : (block_header_t*)ptr - 1;
  size_t before = block == NULL ? 0u : block->allocation.size;
  assert(block == NULL || block->allocation.heap == heap);
  if (size > heap_free[heap] + before) return NULL;
  block = realloc(block, sizeof(*block) + size);
  if (block == NULL) return NULL;
  block->allocation.size = size;
  block->allocation.heap = heap;
  observe_allocation(heap, before, size);
  return block + 1;
}
void* os_malloc(size_t size) { return sdk_realloc(NULL, size, 0u); }
void* os_realloc(void* ptr, size_t size) { return sdk_realloc(ptr, size, 0u); }
void* psram_malloc(size_t size) { return sdk_realloc(NULL, size, 1u); }
void* psram_realloc(void* ptr, size_t size) {
  return sdk_realloc(ptr, size, 1u);
}
void os_free(void* ptr) {
  if (ptr != NULL) {
    block_header_t* block = (block_header_t*)ptr - 1;
    observe_allocation(block->allocation.heap, block->allocation.size, 0u);
    free(block);
  }
}
size_t xPortPointerSize(void* ptr) {
  return ((block_header_t*)ptr - 1)->allocation.size;
}
void h2_bk_memory_acquire(size_t bytes) {
  ++allocations;
  allocation_bytes += bytes;
}
void h2_bk_memory_release(size_t bytes) {
  assert(allocations != 0u && allocation_bytes >= bytes);
  --allocations;
  allocation_bytes -= bytes;
}

static void test_identity(void) {
  const h2_pal_device_identity_api_t* api =
      h2_bk_platform_device_identity_api();
  assert(api == h2_bk_platform_device_identity_api());
  assert(api->vtable->get(api->user, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(mac_calls == 0u);
  h2_pal_device_identity_t identity;
  for (unsigned i = 0u; i < 2u; ++i) {
    memset(&identity, 0xa5, sizeof(identity));
    assert(h2_pal_device_identity_get(api, &identity) == H2_PAL_OK);
    assert(identity.size == 6u && mac_calls == i + 1u);
    assert(memcmp(identity.bytes, bluetooth_mac, 6u) == 0);
    for (size_t j = 6u; j < sizeof(identity.bytes); ++j)
      assert(identity.bytes[j] == 0u);
  }
  mac_result = -1;
  memset(&identity, 0xa5, sizeof(identity));
  assert(api->vtable->get(api->user, &identity) == H2_PAL_ERR_IO);
  const h2_pal_device_identity_t empty = {0};
  assert(memcmp(&identity, &empty, sizeof(identity)) == 0);
  h2_pal_firmware_info_t info;
  assert(h2_pal_firmware_info_get_current(h2_bk_platform_firmware_info_api(),
                                          &info) == H2_PAL_OK);
  assert(strcmp(info.version, "observation-test") == 0);
}

static void assert_heap(const h2_pal_mem_api_t* api, unsigned heap) {
  h2_pal_mem_region_t region =
      heap == 0u ? H2_PAL_MEM_REGION_INTERNAL : H2_PAL_MEM_REGION_EXTERNAL;
  h2_pal_mem_stats_t stats;
  memset(&stats, 0xa5, sizeof(stats));
  unsigned calls = heap_calls[heap];
  unsigned other_calls = heap_calls[1u - heap];
#if !CONFIG_PSRAM_AS_SYS_MEMORY
  if (heap == 1u) {
    const h2_pal_mem_stats_t empty = {0};
    assert(h2_pal_mem_get_stats(api, region, &stats) == H2_PAL_ERR_UNSUPPORTED);
    assert(memcmp(&stats, &empty, sizeof(stats)) == 0);
    assert(heap_calls[heap] == calls && heap_calls[0] == other_calls);
    return;
  }
#endif
  assert(h2_pal_mem_get_stats(api, region, &stats) == H2_PAL_OK);
  assert(heap_calls[heap] == calls + 3u &&
         heap_calls[1u - heap] == other_calls);
  assert(stats.total_bytes == heap_total[heap] &&
         stats.free_bytes == heap_free[heap] &&
         stats.minimum_free_bytes == heap_minimum[heap] &&
         stats.largest_free_block_bytes == 0u &&
         stats.unavailable_fields ==
             H2_PAL_MEM_STATS_UNAVAILABLE_LARGEST_FREE_BLOCK_BYTES);
}

static void test_heap_observations(void) {
  const h2_pal_mem_api_t* apis[] = {
      h2_bk_platform_default_allocator(),
      h2_bk_platform_sram_allocator(),
      h2_bk_platform_psram_allocator(),
  };
  const h2_pal_mem_stats_t empty = {0};
  for (size_t i = 0u; i < sizeof(apis) / sizeof(apis[0]); ++i) {
    const h2_pal_mem_api_t* api = apis[i];
    void* block = h2_pal_mem_alloc(api, 128u);
    assert(block != NULL && allocations == 1u && allocation_bytes == 128u);
    assert_heap(api, 0u);
    assert_heap(api, 1u);
    /* Native users also change the observations, without PAL accounting. */
    void* native = os_malloc(40000u);
    assert(native != NULL && allocations == 1u && allocation_bytes == 128u);
    assert_heap(api, 0u);
    os_free(native);
    assert_heap(api, 0u);
#if CONFIG_PSRAM_AS_SYS_MEMORY
    native = psram_malloc(600000u);
    assert(native != NULL && allocations == 1u && allocation_bytes == 128u);
    assert_heap(api, 1u);
    os_free(native);
    assert_heap(api, 1u);
#endif
    h2_pal_mem_stats_t stats;
    unsigned internal_calls = heap_calls[0], external_calls = heap_calls[1];
    memset(&stats, 0xa5, sizeof(stats));
    assert(api->vtable->get_stats(api->user,
                                  H2_PAL_MEM_REGION_INTERNAL_EXECUTABLE,
                                  &stats) == H2_PAL_ERR_UNSUPPORTED);
    assert(memcmp(&stats, &empty, sizeof(stats)) == 0);
    memset(&stats, 0xa5, sizeof(stats));
    assert(api->vtable->get_stats(api->user, (h2_pal_mem_region_t)99, &stats) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(memcmp(&stats, &empty, sizeof(stats)) == 0);
    assert(api->vtable->get_stats(api->user, H2_PAL_MEM_REGION_INTERNAL,
                                  NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(heap_calls[0] == internal_calls && heap_calls[1] == external_calls);
    for (unsigned heap = 0u; heap < 2u; ++heap) {
      /* An absent native region must not appear as a successful zero heap. */
      size_t total = heap_total[heap];
      heap_total[heap] = 0u;
      memset(&stats, 0xa5, sizeof(stats));
      h2_pal_mem_region_t region =
          heap == 0u ? H2_PAL_MEM_REGION_INTERNAL : H2_PAL_MEM_REGION_EXTERNAL;
      assert(api->vtable->get_stats(api->user, region, &stats) ==
             H2_PAL_ERR_UNSUPPORTED);
      assert(memcmp(&stats, &empty, sizeof(stats)) == 0);
      heap_total[heap] = total;
    }
    h2_pal_mem_free(api, block);
    assert(allocations == 0u && allocation_bytes == 0u);
  }
}

int main(void) {
  test_identity();
  test_heap_observations();
  return 0;
}
