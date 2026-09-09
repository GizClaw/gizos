#ifndef H2_ESP_SIMCOM_URC_H
#define H2_ESP_SIMCOM_URC_H

#include "esp_err.h"
#include "h2/pal/core/h2_pal_errors.h"
#include <stddef.h>
#include <stdint.h>

typedef h2_pal_result_t (*h2_esp_simcom_post_urc_fn)(void *user, const char *line);

/* Forward a receive buffer without clipping lines or hiding delivery errors.
 * The caller owns fragmentation; each line must be complete on entry. */
esp_err_t h2_esp_simcom_forward_urcs(
    void *user, const uint8_t *data, size_t len, h2_esp_simcom_post_urc_fn post);

#endif
