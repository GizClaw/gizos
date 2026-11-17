#ifndef H2_PAL_MEM_H
#define H2_PAL_MEM_H

#include "core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_pal_mem_api h2_pal_mem_api_t;

typedef enum h2_pal_mem_region {
  H2_PAL_MEM_REGION_INTERNAL = 0,
  H2_PAL_MEM_REGION_INTERNAL_EXECUTABLE = 1,
  H2_PAL_MEM_REGION_EXTERNAL = 2,
} h2_pal_mem_region_t;

typedef enum h2_pal_mem_stats_unavailable_field {
  H2_PAL_MEM_STATS_UNAVAILABLE_TOTAL_BYTES = 1u << 0,
  H2_PAL_MEM_STATS_UNAVAILABLE_FREE_BYTES = 1u << 1,
  H2_PAL_MEM_STATS_UNAVAILABLE_MINIMUM_FREE_BYTES = 1u << 2,
  H2_PAL_MEM_STATS_UNAVAILABLE_LARGEST_FREE_BLOCK_BYTES = 1u << 3,
} h2_pal_mem_stats_unavailable_field_t;

/** Whole-region heap observations, independent of this allocator's counters.
 * Regions may overlap. Values are observational and reserve no memory.
 * An unavailable numeric field is zero with its corresponding
 * unavailable_fields bit set. A zero mask means every field was observed; an
 * observed zero remains distinguishable from an unavailable value. Callers must
 * inspect the mask even on OK. Providers return UNSUPPORTED when the region has
 * no observations. The unavailable_fields tail changes the binary layout:
 * rebuild providers and consumers together; this structure does not preserve
 * the earlier binary ABI.
 */
typedef struct h2_pal_mem_stats {
  size_t total_bytes;
  size_t free_bytes;
  size_t minimum_free_bytes;
  size_t largest_free_block_bytes;
  uint32_t unavailable_fields;
} h2_pal_mem_stats_t;

typedef struct h2_pal_mem_vtable {
  void* (*alloc)(void* user, size_t len);
  void* (*realloc)(void* user, void* ptr, size_t len);
  void (*free)(void* user, void* ptr);
  /** Return OK for available region observations, marking unavailable fields.
   * Return UNSUPPORTED when whole-region observations are unavailable. */
  h2_pal_result_t (*get_stats)(void* user, h2_pal_mem_region_t region,
                               h2_pal_mem_stats_t* out_stats);
} h2_pal_mem_vtable_t;

struct h2_pal_mem_api {
  void* user;
  const h2_pal_mem_vtable_t* vtable;
};

static inline void* h2_pal_mem_alloc(const h2_pal_mem_api_t* api, size_t len) {
  if (api == NULL || api->vtable == NULL || api->vtable->alloc == NULL) {
    return NULL;
  }
  return api->vtable->alloc(api->user, len);
}

static inline void* h2_pal_mem_realloc(const h2_pal_mem_api_t* api, void* ptr,
                                       size_t len) {
  if (api == NULL || api->vtable == NULL || api->vtable->realloc == NULL) {
    return NULL;
  }
  return api->vtable->realloc(api->user, ptr, len);
}

static inline void h2_pal_mem_free(const h2_pal_mem_api_t* api, void* ptr) {
  if (api == NULL || api->vtable == NULL || api->vtable->free == NULL ||
      ptr == NULL) {
    return;
  }
  api->vtable->free(api->user, ptr);
}

/** Output is cleared on failure. On OK, unavailable_fields distinguishes
 * missing observations from numeric zero. */
static inline h2_pal_result_t h2_pal_mem_get_stats(
    const h2_pal_mem_api_t* api, h2_pal_mem_region_t region,
    h2_pal_mem_stats_t* out_stats) {
  h2_pal_result_t rc;
  if (out_stats == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_stats, 0, sizeof(*out_stats));
  if (region != H2_PAL_MEM_REGION_INTERNAL &&
      region != H2_PAL_MEM_REGION_INTERNAL_EXECUTABLE &&
      region != H2_PAL_MEM_REGION_EXTERNAL)
    return H2_PAL_ERR_INVALID_ARG;
  if (api == NULL || api->vtable == NULL || api->vtable->get_stats == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  rc = api->vtable->get_stats(api->user, region, out_stats);
  if (rc != H2_PAL_OK) memset(out_stats, 0, sizeof(*out_stats));
  return rc;
}

#ifdef __cplusplus
}
#endif

#endif
