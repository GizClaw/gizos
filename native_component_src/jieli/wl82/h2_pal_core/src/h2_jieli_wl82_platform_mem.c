#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_sdk_port.h"

static void* mem_alloc(void* user, size_t length) {
  (void)user;
  if (length == 0u) {
    return NULL;
  }
  return h2_jieli_sdk_malloc(length);
}

static void* mem_realloc(void* user, void* ptr, size_t length) {
  (void)user;
  if (length == 0u) {
    h2_jieli_sdk_free(ptr);
    return NULL;
  }
  return h2_jieli_sdk_realloc(ptr, length);
}

static void mem_free(void* user, void* ptr) {
  (void)user;
  if (ptr != NULL) {
    h2_jieli_sdk_free(ptr);
  }
}

static h2_pal_result_t mem_get_stats(void* user, h2_pal_mem_region_t region,
                                     h2_pal_mem_stats_t* out_stats) {
  (void)user;
  if (out_stats == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_stats, 0, sizeof(*out_stats));
  if (region != H2_PAL_MEM_REGION_INTERNAL &&
      region != H2_PAL_MEM_REGION_INTERNAL_EXECUTABLE &&
      region != H2_PAL_MEM_REGION_EXTERNAL)
    return H2_PAL_ERR_INVALID_ARG;
  /* The wl82 SDK does not expose all four whole-region observations. */
  return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_mem_vtable_t s_mem_vtable = {
    .alloc = mem_alloc,
    .realloc = mem_realloc,
    .free = mem_free,
    .get_stats = mem_get_stats,
};

static const h2_pal_mem_api_t s_mem_api = {
    .user = NULL,
    .vtable = &s_mem_vtable,
};

const h2_pal_mem_api_t* h2_jieli_wl82_platform_mem_api(void) {
  return &s_mem_api;
}
