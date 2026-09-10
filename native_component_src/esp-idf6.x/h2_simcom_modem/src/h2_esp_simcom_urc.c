#include "h2_esp_simcom_urc.h"
#include "h2_simcom_modem.h"

#include <string.h>

esp_err_t h2_esp_simcom_forward_urcs(
    void *user, const uint8_t *data, size_t len, h2_esp_simcom_post_urc_fn post) {
    if (data == NULL || len == 0u) {
        return ESP_ERR_NOT_FOUND;
    }
    if (post == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Validate the entire buffer before delivering any of it. A UART chunk
     * can contain several short lines even when its total size exceeds 192. */
    size_t line_len = 0u;
    for (size_t i = 0u; i < len; ++i) {
        if (data[i] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        if (data[i] == '\r' || data[i] == '\n') {
            line_len = 0u;
        } else if (++line_len >= H2_SIMCOM_LINE_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    size_t offset = 0u;
    while (offset < len) {
        if (data[offset] == '\r' || data[offset] == '\n') {
            offset++;
            continue;
        }
        size_t end = offset;
        while (end < len && data[end] != '\r' && data[end] != '\n') { end++; }
        char line[H2_SIMCOM_LINE_MAX];
        memcpy(line, data + offset, end - offset);
        line[end - offset] = '\0';
        h2_pal_result_t rc = post(user, line);
        if (rc != H2_PAL_OK) {
            switch (rc) {
                case H2_PAL_ERR_FULL:
                case H2_PAL_ERR_NO_MEMORY:
                    return ESP_ERR_NO_MEM;
                case H2_PAL_ERR_TRUNCATED:
                    return ESP_ERR_INVALID_SIZE;
                case H2_PAL_ERR_INVALID_ARG:
                    return ESP_ERR_INVALID_ARG;
                case H2_PAL_ERR_INVALID_STATE:
                case H2_PAL_ERR_CLOSED:
                    return ESP_ERR_INVALID_STATE;
                default:
                    return ESP_FAIL;
            }
        }
        offset = end;
    }
    return ESP_OK;
}
