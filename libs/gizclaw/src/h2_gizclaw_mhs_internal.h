#ifndef H2_GIZCLAW_MHS_INTERNAL_H
#define H2_GIZCLAW_MHS_INTERNAL_H

#include "h2_gizclaw_config.h"
#include "h2_runtime.h"

typedef struct h2_gizclaw_mhs_builtin {
  const h2_pal_audio_api_t *audio;
  const h2_pal_wifi_sta_api_t *wifi;
  h2_runtime_t *runtime;
} h2_gizclaw_mhs_builtin_t;

/* Caller provides seven slots, retained along with context for the table's
 * lifetime. Only available PAL capabilities are installed. */
size_t h2_gizclaw_mhs_builtins_internal(h2_gizclaw_mhs_builtin_t *context,
                                        h2_gizclaw_mhs_state_t *states);
int h2_gizclaw_mhs_validate_internal(const h2_gizclaw_mhs_state_t *states,
                                     size_t count);
/* On success the caller owns *storage through its allocator and must keep it
 * alive while consuming response. On failure *storage is NULL. */
int h2_gizclaw_mhs_request_internal(
    const h2_gizclaw_mhs_state_t *states, size_t count, bool write,
    const h2_pal_mem_api_t *allocator, h2_gizclaw_rpc_bytes_t request,
    h2_gizclaw_rpc_provider_response_t *response, uint8_t **storage);
#endif
