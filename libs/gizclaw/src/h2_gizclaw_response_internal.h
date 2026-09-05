#ifndef H2_GIZCLAW_RESPONSE_INTERNAL_H
#define H2_GIZCLAW_RESPONSE_INTERNAL_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2_gizclaw_types.h"

#include <stdbool.h>

typedef struct h2_gizclaw_resp_arena {
  h2_pal_mem_api_t allocator;
  h2_gizclaw_resp_storage_t *storage;
  size_t checkpoint;
  bool exhausted;
} h2_gizclaw_resp_arena_t;

h2_pal_result_t h2_gizclaw_resp_arena_begin(h2_gizclaw_resp_storage_t *storage,
                                            h2_gizclaw_resp_arena_t *arena);
h2_pal_result_t h2_gizclaw_resp_arena_end(h2_gizclaw_resp_arena_t *arena,
                                          h2_pal_result_t result);

#endif
