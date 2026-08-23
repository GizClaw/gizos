#ifndef H2_SMOKE_WIFI_CSI_H
#define H2_SMOKE_WIFI_CSI_H

#include "h2_runtime.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Receives the result of the first dashboard render. */
typedef void (*h2_smoke_wifi_csi_ready_fn)(void *user, int result);

/** Returns nonzero when the app main loop should stop. */
typedef int (*h2_smoke_wifi_csi_should_stop_fn)(void *user);

/** Configures the portable Wi-Fi CSI dashboard. */
typedef struct h2_smoke_wifi_csi_config {
  void *user;
  h2_smoke_wifi_csi_ready_fn ready;
  h2_smoke_wifi_csi_should_stop_fn should_stop;
  uint32_t wifi_connect_timeout_ms;
  uint32_t retry_interval_ms;
  uint32_t refresh_interval_ms;
} h2_smoke_wifi_csi_config_t;

/**
 * Runs the raw Wi-Fi CSI dashboard until should_stop requests exit or rendering
 * fails. It does not infer occupancy, sleep, gesture, posture, location, or
 * any other human state.
 *
 * @param runtime Required portable Runtime with display, Wi-Fi, CSI, time,
 *                synchronization, and memory capabilities.
 * @param config Required dashboard configuration, borrowed for this call.
 * @return H2_PAL_OK after an intentional stop; otherwise the first error.
 */
int h2_smoke_wifi_csi_run(h2_runtime_t *runtime,
                          const h2_smoke_wifi_csi_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
