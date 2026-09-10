#include "h2_quectel_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static h2_pal_result_t qlock(h2_quectel_modem_t *modem) {
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (modem->lock == NULL) {
        return H2_PAL_OK;
    }
    return h2_pal_mutex_lock(modem->config.sync_api, modem->lock);
}

static void qunlock(h2_quectel_modem_t *modem) {
    if (modem != NULL && modem->lock != NULL) {
        (void)h2_pal_mutex_unlock(modem->config.sync_api, modem->lock);
    }
}

static void trim_line(char *line) {
    size_t len = strlen(line);
    while (len > 0u && (line[len - 1u] == '\r' || line[len - 1u] == '\n' || h2_quectel_ascii_space((unsigned char)line[len - 1u]))) {
        line[--len] = '\0';
    }
    size_t start = 0u;
    while (line[start] != '\0' && h2_quectel_ascii_space((unsigned char)line[start])) {
        start++;
    }
    if (start > 0u) {
        memmove(line, line + start, strlen(line + start) + 1u);
    }
}

static h2_pal_result_t write_all(h2_quectel_modem_t *modem, const char *data, uint32_t timeout_ms) {
    size_t len = strlen(data);
    size_t written = 0u;
    h2_pal_result_t rc = modem->config.write(modem->config.transport_user, (const uint8_t *)data, len, timeout_ms, &written);
    return rc == H2_PAL_OK && written == len ? H2_PAL_OK : (rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc);
}

static h2_pal_result_t read_line(h2_quectel_modem_t *modem, char *line, size_t cap, uint32_t timeout_ms) {
    if (line == NULL || cap == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    size_t len = 0u;
    line[0] = '\0';
    for (;;) {
        uint8_t ch = 0u;
        size_t got = 0u;
        h2_pal_result_t rc = modem->config.read(modem->config.transport_user, &ch, 1u, timeout_ms, &got);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (got == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
        if (len + 1u < cap) {
            line[len++] = (char)ch;
        }
        if (ch == '\n') {
            line[len] = '\0';
            trim_line(line);
            return H2_PAL_OK;
        }
    }
}

static void response_add_line(h2_quectel_response_t *response, const char *line) {
    if (response == NULL || line == NULL || line[0] == '\0' || response->count >= H2_QUECTEL_RESPONSE_MAX) {
        return;
    }
    strncpy(response->lines[response->count], line, H2_QUECTEL_LINE_MAX - 1u);
    response->lines[response->count][H2_QUECTEL_LINE_MAX - 1u] = '\0';
    response->count++;
}

static void response_add_text(h2_quectel_modem_t *modem, h2_quectel_response_t *response, const char *text, const char *cmd) {
    if (text == NULL) {
        return;
    }

    const char *cursor = text;
    while (*cursor != '\0') {
        char line[H2_QUECTEL_LINE_MAX];
        size_t len = 0u;
        while (cursor[len] != '\0' && cursor[len] != '\r' && cursor[len] != '\n' && len + 1u < sizeof(line)) {
            line[len] = cursor[len];
            len++;
        }
        line[len] = '\0';
        size_t consume = len;
        while (cursor[consume] != '\0' && cursor[consume] != '\r' && cursor[consume] != '\n') {
            consume++;
        }
        while (cursor[consume] == '\r' || cursor[consume] == '\n') {
            consume++;
        }
        cursor += consume;

        trim_line(line);
        if (line[0] == '\0' || strcmp(line, "OK") == 0 || strcmp(line, "ERROR") == 0 || (cmd != NULL && strcmp(line, cmd) == 0)) {
            continue;
        }
        h2_quectel_handle_urc_locked(modem, line);
        response_add_line(response, line);
    }
}

static int response_text_has_connect(const char *text) {
    if (text == NULL) {
        return 0;
    }
    const char *cursor = text;
    while (*cursor != '\0') {
        char line[H2_QUECTEL_LINE_MAX];
        size_t len = 0u;
        while (cursor[len] != '\0' && cursor[len] != '\r' && cursor[len] != '\n' && len + 1u < sizeof(line)) {
            line[len] = cursor[len];
            len++;
        }
        line[len] = '\0';
        while (cursor[len] == '\r' || cursor[len] == '\n') {
            len++;
        }
        cursor += len;
        trim_line(line);
        if (strcmp(line, "CONNECT") == 0) {
            return 1;
        }
    }
    return 0;
}

h2_pal_result_t h2_quectel_at_exchange_locked(
    h2_quectel_modem_t *modem,
    const char *cmd,
    h2_quectel_response_t *response,
    int allow_connect) {
    if (modem == NULL || cmd == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (response != NULL) {
        memset(response, 0, sizeof(*response));
    }

    if (modem->config.command != NULL) {
        char command_response[H2_QUECTEL_LINE_MAX * H2_QUECTEL_RESPONSE_MAX];
        memset(command_response, 0, sizeof(command_response));
        h2_pal_result_t rc = modem->config.command(
            modem->config.transport_user,
            cmd,
            command_response,
            sizeof(command_response),
            modem->config.command_timeout_ms);
        if (allow_connect != 0 && response_text_has_connect(command_response)) {
            if (response != NULL) {
                response->connected = 1;
            }
            response_add_text(modem, response, command_response, cmd);
            return H2_PAL_OK;
        }
        response_add_text(modem, response, command_response, cmd);
        return rc;
    }
    if (modem->config.read == NULL || modem->config.write == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }

    char tx[H2_QUECTEL_LINE_MAX];
    int n = snprintf(tx, sizeof(tx), "%s\r", cmd);
    if (n <= 0 || (size_t)n >= sizeof(tx)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = write_all(modem, tx, modem->config.io_timeout_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }

    for (uint32_t i = 0u; i < 128u; ++i) {
        char line[H2_QUECTEL_LINE_MAX];
        rc = read_line(modem, line, sizeof(line), modem->config.io_timeout_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (line[0] == '\0' || strcmp(line, cmd) == 0) {
            continue;
        }
        if (strcmp(line, "OK") == 0) {
            return H2_PAL_OK;
        }
        if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR:", 11) == 0 || strncmp(line, "+CMS ERROR:", 11) == 0) {
            h2_quectel_handle_urc_locked(modem, line);
            if (response != NULL) {
                response_add_line(response, line);
            }
            return H2_PAL_ERR_IO;
        }
        if (allow_connect != 0 && strcmp(line, "CONNECT") == 0) {
            if (response != NULL) {
                response->connected = 1;
            }
            return H2_PAL_OK;
        }
        h2_quectel_handle_urc_locked(modem, line);
        if (response != NULL) {
            response_add_line(response, line);
        }
    }
    return H2_PAL_ERR_TIMEOUT;
}

h2_pal_result_t h2_quectel_at_exchange_timeout(
    h2_quectel_modem_t *modem,
    const char *cmd,
    h2_quectel_response_t *response,
    int allow_connect,
    uint32_t timeout_ms) {
    h2_pal_result_t rc = qlock(modem);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    /* The timeouts live in the config the exchange reads, so they are swapped
     * under the AT lock and restored before another command can observe them. */
    const uint32_t saved_command_timeout_ms = modem->config.command_timeout_ms;
    const uint32_t saved_io_timeout_ms = modem->config.io_timeout_ms;
    if (timeout_ms != 0u) {
        modem->config.command_timeout_ms = timeout_ms;
        modem->config.io_timeout_ms = timeout_ms;
    }
    if (response != NULL) {
        memset(response, 0, sizeof(*response));
    }
    rc = h2_quectel_power_wake(modem);
    /* Do not flush here: unsolicited SIM/call notifications must survive. */
    if (rc == H2_PAL_OK) {
        rc = h2_quectel_at_exchange_locked(modem, cmd, response, allow_connect);
    }
    /* A terminal CME response (for example GNSS has no fix) is not an
     * uncertain transport failure. Session holds still survive failed stops. */
    int terminal_error = response != NULL &&
        (h2_quectel_response_find(response, "+CME ERROR:") != NULL ||
         h2_quectel_response_find(response, "+CMS ERROR:") != NULL);
    if (rc != H2_PAL_OK && !terminal_error &&
        (modem->capabilities & H2_PAL_MODEM_CAPABILITY_LOW_POWER) != 0u) {
        modem->power_fault = 1u;
    }
    modem->config.command_timeout_ms = saved_command_timeout_ms;
    modem->config.io_timeout_ms = saved_io_timeout_ms;
    qunlock(modem);
    return rc;
}

h2_pal_result_t h2_quectel_at_exchange(
    h2_quectel_modem_t *modem,
    const char *cmd,
    h2_quectel_response_t *response,
    int allow_connect) {
    return h2_quectel_at_exchange_timeout(modem, cmd, response, allow_connect, 0u);
}

const char *h2_quectel_response_find(const h2_quectel_response_t *response, const char *prefix) {
    if (response == NULL || prefix == NULL) {
        return NULL;
    }
    size_t prefix_len = strlen(prefix);
    for (size_t i = 0u; i < response->count; ++i) {
        if (strncmp(response->lines[i], prefix, prefix_len) == 0) {
            return response->lines[i];
        }
    }
    return NULL;
}

int h2_quectel_parse_int_after(const char *text, const char *prefix, int *out_value) {
    if (text == NULL || prefix == NULL || out_value == NULL) {
        return 0;
    }
    size_t prefix_len = strlen(prefix);
    if (strncmp(text, prefix, prefix_len) != 0) {
        return 0;
    }
    char *end = NULL;
    long value = strtol(text + prefix_len, &end, 10);
    if (end == text + prefix_len) {
        return 0;
    }
    *out_value = (int)value;
    return 1;
}

void h2_quectel_copy_token(char *dst, size_t dst_len, const char *src) {
    if (dst == NULL || dst_len == 0u) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }
    while (*src == ' ' || *src == '"' || *src == ',') {
        src++;
    }
    size_t i = 0u;
    while (src[i] != '\0' && src[i] != '"' && src[i] != ',' && src[i] != '\r' && src[i] != '\n' && i + 1u < dst_len) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}
