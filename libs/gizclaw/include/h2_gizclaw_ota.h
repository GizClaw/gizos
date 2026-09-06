#ifndef H2_GIZCLAW_OTA_H
#define H2_GIZCLAW_OTA_H
#include "h2_gizclaw_firmware.h"
#ifdef __cplusplus
extern "C" {
#endif
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
