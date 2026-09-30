#ifndef H2_GIZCLAW_MHS_INTERNAL_H
#define H2_GIZCLAW_MHS_INTERNAL_H

#include "h2_gizclaw_config.h"
#include "h2_runtime.h"

typedef struct h2_gizclaw_mhs_builtin {
  const h2_pal_audio_api_t *audio;
  const h2_pal_wifi_sta_api_t *wifi;
  h2_runtime_t *runtime;
  const h2_pal_modem_api_t *modem;
  h2_runtime_component_id_t battery;
} h2_gizclaw_mhs_builtin_t;

#define H2_GIZCLAW_MHS_BUILTIN_MAX 4u

/* Up to H2_GIZCLAW_MHS_BUILTIN_MAX instances: speaker.main and wifi.main from
 * their PAL, modem.main and battery.main from Runtime snapshots. */
size_t h2_gizclaw_mhs_builtins_internal(h2_gizclaw_mhs_builtin_t *context,
                                        h2_gizclaw_mhs_device_t *devices);
int h2_gizclaw_mhs_validate_internal(const h2_gizclaw_mhs_device_t *devices,
                                     size_t count);
int h2_gizclaw_mhs_request_internal(
    const h2_gizclaw_mhs_device_t *devices, size_t count, bool write,
    const h2_pal_mem_api_t *allocator, h2_gizclaw_rpc_bytes_t request,
    h2_gizclaw_rpc_provider_response_t *response, uint8_t **storage);
/* H2_GIZCLAW_RPC_ERROR_* for a failed h2_gizclaw_mhs_request_internal(). */
int h2_gizclaw_mhs_error_internal(int result);
#endif
