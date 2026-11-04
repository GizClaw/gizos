#include "h2_bk_platform_core.h"
#include "h2_bk_resource_stats_internal.h"
#include "components/system.h"
#include "os/mem.h"

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

typedef union block_header {
#if defined(_MSC_VER) && !defined(__clang__)
  /* MSVC's C headers omit max_align_t; cover its scalar alignments. */
  long double alignment;
  long long integer_alignment;
  void* pointer_alignment;
#else
  max_align_t alignment;
#endif
  size_t size;
} block_header_t;

int bk_get_mac(uint8_t* mac, mac_type_t type) {
  assert(type == MAC_TYPE_BLUETOOTH);
  ++mac_calls;
  memcpy(mac, bluetooth_mac, sizeof(bluetooth_mac));
  return mac_result;
}

void* os_malloc(size_t size) {
  block_header_t* block = malloc(sizeof(*block) + size);
  if (block == NULL) return NULL;
  block->size = size;
  return block + 1;
}
void* os_realloc(void* ptr, size_t size) {
  block_header_t* block = ptr == NULL ? NULL : (block_header_t*)ptr - 1;
  block = realloc(block, sizeof(*block) + size);
  if (block == NULL) return NULL;
  block->size = size;
  return block + 1;
}
void* psram_malloc(size_t size) { return os_malloc(size); }
void* psram_realloc(void* ptr, size_t size) { return os_realloc(ptr, size); }
void os_free(void* ptr) {
  if (ptr != NULL) free((block_header_t*)ptr - 1);
}
size_t xPortPointerSize(void* ptr) { return ((block_header_t*)ptr - 1)->size; }
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

static void test_heap_observations_are_explicitly_unavailable(void) {
  const h2_pal_mem_api_t* apis[] = {
      h2_bk_platform_default_allocator(),
      h2_bk_platform_sram_allocator(),
      h2_bk_platform_psram_allocator(),
  };
  const h2_pal_mem_region_t regions[] = {
      H2_PAL_MEM_REGION_INTERNAL,
      H2_PAL_MEM_REGION_INTERNAL_EXECUTABLE,
      H2_PAL_MEM_REGION_EXTERNAL,
  };
  const h2_pal_mem_stats_t empty = {0};
  for (size_t i = 0u; i < sizeof(apis) / sizeof(apis[0]); ++i) {
    const h2_pal_mem_api_t* api = apis[i];
    void* block = h2_pal_mem_alloc(api, 128u);
    assert(block != NULL && allocations == 1u && allocation_bytes == 128u);
    for (size_t j = 0u; j < sizeof(regions) / sizeof(regions[0]); ++j) {
      h2_pal_mem_stats_t stats;
      memset(&stats, 0xa5, sizeof(stats));
      assert(h2_pal_mem_get_stats(api, regions[j], &stats) ==
             H2_PAL_ERR_UNSUPPORTED);
      assert(memcmp(&stats, &empty, sizeof(stats)) == 0);
      assert(allocations == 1u && allocation_bytes == 128u);
    }
    h2_pal_mem_stats_t stats;
    assert(api->vtable->get_stats(api->user, (h2_pal_mem_region_t)99, &stats) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(api->vtable->get_stats(api->user, regions[0], NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    h2_pal_mem_free(api, block);
    assert(allocations == 0u && allocation_bytes == 0u);
  }
}

int main(void) {
  test_identity();
  test_heap_observations_are_explicitly_unavailable();
  return 0;
}
