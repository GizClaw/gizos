#include "h2_android_platform.h"
#include "h2_wolfssl.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_size_t allocations;
static void* allocate(void* user, size_t len) {
  (void)user;
  void* memory = malloc(len == 0u ? 1u : len);
  if (memory != NULL) atomic_fetch_add(&allocations, 1u);
  return memory;
}
static void release(void* user, void* memory) {
  (void)user;
  if (memory != NULL) {
    assert(atomic_fetch_sub(&allocations, 1u) > 0u);
    free(memory);
  }
}
static void* resize(void* user, void* memory, size_t len) {
  if (memory == NULL) return allocate(user, len);
  if (len == 0u) {
    release(user, memory);
    return NULL;
  }
  return realloc(memory, len);
}
const h2_pal_mem_api_t* h2_android_platform_mem_api(void) {
  static const h2_pal_mem_vtable_t methods = {
      .alloc = allocate, .realloc = resize, .free = release};
  static const h2_pal_mem_api_t api = {.vtable = &methods};
  return &api;
}
static h2_pal_result_t monotonic(void* user, uint64_t* out) {
  (void)user;
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return H2_PAL_ERR_IO;
  *out = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
  return H2_PAL_OK;
}
const h2_pal_time_api_t* h2_android_platform_time_api(void) {
  static const h2_pal_time_vtable_t methods = {.get_monotonic_ms = monotonic};
  static const h2_pal_time_api_t api = {.vtable = &methods};
  return &api;
}
static int different_entropy(void* user, uint8_t* out, size_t len) {
  (void)user;
  memset(out, 0, len);
  return H2_PAL_OK;
}
int main(void) {
  h2_android_http_t* http = NULL;
  assert(h2_android_http_create(NULL, 0u, &http) == H2_PAL_OK);
  assert(http != NULL);
  /* Native timeout tests hold the real gated setup. At this adapter boundary,
   * retain its operation lease while destroying the actual HTTP owner. */
  assert(h2_wolfssl_operation_acquire_concurrent() == H2_PAL_OK);
  h2_android_http_destroy(http);
  http = NULL;
  assert(h2_wolfssl_operation_acquire() == H2_PAL_ERR_INVALID_STATE);
  h2_wolfssl_operation_release();
  assert(atomic_load(&allocations) == 0u);

  const h2_wolfssl_config_t other = {.mem = *h2_android_platform_mem_api(),
                                     .entropy = different_entropy,
                                     .providers_thread_safe = true};
  assert(h2_wolfssl_init(&other) == H2_PAL_OK);
  assert(h2_wolfssl_deinit() == H2_PAL_OK);
  assert(atomic_load(&allocations) == 0u);
  for (unsigned i = 0u; i < 3u; ++i) {
    assert(h2_android_http_create(NULL, 0u, &http) == H2_PAL_OK);
    h2_android_http_destroy(http);
    http = NULL;
    assert(atomic_load(&allocations) == 0u);
  }
  return 0;
}
