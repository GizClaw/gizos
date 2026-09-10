#ifndef H2_GIZCLAW_OTA_H
#define H2_GIZCLAW_OTA_H
#include "h2_gizclaw_firmware.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum h2_gizclaw_ota_phase {
  H2_GIZCLAW_OTA_IDLE = 0,
  H2_GIZCLAW_OTA_RUNNING,
  H2_GIZCLAW_OTA_STAGED,
  H2_GIZCLAW_OTA_FAILED,
} h2_gizclaw_ota_phase_t;

typedef struct h2_gizclaw_ota_status {
  h2_gizclaw_ota_phase_t phase;
  h2_pal_result_t result;
} h2_gizclaw_ota_status_t;

/** Copy the latest local attempt status under the device mutex. No network I/O.
 * Safe alongside start and device callbacks while the Service is alive.
 * STAGED means activation was accepted, never post-boot success. Status resets
 * when the Service is recreated. Failure clears the caller's output.
 */
h2_pal_result_t h2_gizclaw_ota_get_status(h2_gizclaw_service_t *service,
                                         h2_gizclaw_ota_status_t *out_status);

/** Begin OTA asynchronously, using firmware_get, HTTPS download, the configured
 * staging vtable and OTA telemetry. A zero channel uses
 * config.firmware_channel. An empty expected_sha256 accepts the fetched
 * metadata's hash; a supplied hash pins a previously checked firmware.
 * Arguments are copied before return. OK means accepted. Installing/booting and
 * post-boot success are distinct. */
h2_pal_result_t h2_gizclaw_ota_start(h2_gizclaw_service_t *service,
                                     int32_t channel,
                                     h2_gizclaw_str_t expected_sha256);
#ifdef __cplusplus
}
#endif
#endif
