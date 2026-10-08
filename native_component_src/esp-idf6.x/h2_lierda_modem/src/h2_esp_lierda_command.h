#ifndef H2_ESP_LIERDA_COMMAND_H
#define H2_ESP_LIERDA_COMMAND_H

#include "h2/pal/core/h2_pal_errors.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_modem_c_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_esp_lierda_command_status {
    esp_err_t sdk_error;
    int32_t cme_error; /* Strict decimal final CME 0..65535, else -1/unknown. */
    bool command_started;
    bool final_result;
} h2_esp_lierda_command_status_t;

/* Private SDK bridge. Response storage is borrowed only through this blocking
 * call. The pinned DTE clears its callback under its line lock before return,
 * including timeout; no SDK task can retain the captured storage afterwards. */
h2_pal_result_t h2_esp_lierda_command(
    esp_modem_dce_t *dce, const char *command, char *response,
    size_t capacity, uint32_t timeout_ms, h2_esp_lierda_command_status_t *out_status);

#ifdef __cplusplus
}
#endif
#endif
