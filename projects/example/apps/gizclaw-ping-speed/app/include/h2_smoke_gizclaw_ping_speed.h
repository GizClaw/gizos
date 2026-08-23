#ifndef H2_SMOKE_GIZCLAW_PING_SPEED_H
#define H2_SMOKE_GIZCLAW_PING_SPEED_H

#include "h2_gizclaw_client.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_smoke_gizclaw_result {
    H2_SMOKE_GIZCLAW_OK = 0,
    H2_SMOKE_GIZCLAW_SKIP = 1,
    H2_SMOKE_GIZCLAW_FAIL = 2,
} h2_smoke_gizclaw_result_t;

typedef struct h2_smoke_gizclaw_ping_speed_config {
    h2_gizclaw_str_t server_endpoint;
    h2_gizclaw_str_t private_key;
    h2_gizclaw_cipher_mode_t cipher_mode;
    uint32_t connect_timeout_ms;
    uint32_t wifi_connect_timeout_ms;
    uint32_t server_info_timeout_ms;
    uint32_t poll_window_ms;
} h2_smoke_gizclaw_ping_speed_config_t;

h2_smoke_gizclaw_result_t h2_smoke_gizclaw_ping_speed_run(
    h2_runtime_t *runtime,
    const h2_smoke_gizclaw_ping_speed_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
