#include "h2_lierda_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static const char *skip_space(const char *value) {
    while (*value == ' ' || *value == '\t') ++value;
    return value;
}

bool h2_lierda_decimal(const char **cursor, unsigned *out_value) {
    const char *p = skip_space(*cursor);
    if (*p < '0' || *p > '9') return false;
    unsigned n = 0u;
    do {
        const unsigned digit = (unsigned)(*p++ - '0');
        if (n > (UINT_MAX - digit) / 10u) return false;
        n = n * 10u + digit;
    } while (*p >= '0' && *p <= '9');
    *cursor = skip_space(p);
    *out_value = n;
    return true;
}

bool h2_lierda_comma(const char **cursor) {
    const char *p = skip_space(*cursor);
    if (*p != ',') return false;
    *cursor = skip_space(p + 1);
    return true;
}

bool h2_lierda_end(const char *cursor) {
    return *skip_space(cursor) == '\0';
}

h2_pal_result_t h2_lierda_command(
    h2_lierda_modem_t *modem, const char *command, char *response) {
    memset(response, 0, H2_LIERDA_RESPONSE_SIZE);
    /* Sentinel detects a callback that fills the entire buffer without NUL. */
    response[H2_LIERDA_RESPONSE_SIZE - 1u] = '\x7f';
    h2_pal_result_t rc = modem->config.transport.command(
        modem->config.transport.user, command, response,
        H2_LIERDA_RESPONSE_SIZE,
        modem->config.command_timeout_ms != 0u
            ? modem->config.command_timeout_ms : 5000u);
    if (rc != H2_PAL_OK) return rc;
    if (memchr(response, '\0', H2_LIERDA_RESPONSE_SIZE) == NULL) {
        return H2_PAL_ERR_TRUNCATED;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_lierda_response_line(
    const char *response, const char *prefix, char *out_line, size_t capacity) {
    size_t matches = 0u;
    out_line[0] = '\0';
    const size_t prefix_len = prefix != NULL ? strlen(prefix) : 0u;
    for (const char *p = response; *p != '\0';) {
        const char *end = p;
        while (*end != '\0' && *end != '\r' && *end != '\n') ++end;
        const char *start = skip_space(p);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;
        const size_t len = end > start ? (size_t)(end - start) : 0u;
        const bool terminal = (len == 2u && memcmp(start, "OK", 2u) == 0);
        if (len != 0u && !terminal &&
            (prefix == NULL ||
             (len >= prefix_len && memcmp(start, prefix, prefix_len) == 0))) {
            if (++matches != 1u) return H2_PAL_ERR_FORMAT;
            const size_t copy_len = len - prefix_len;
            if (copy_len >= capacity) return H2_PAL_ERR_TRUNCATED;
            memcpy(out_line, start + prefix_len, copy_len);
            out_line[copy_len] = '\0';
        }
        p = end;
        /* end may have moved back over spaces; still consume the whole line. */
        while (*p != '\0' && *p != '\r' && *p != '\n') ++p;
        while (*p == '\r' || *p == '\n') ++p;
    }
    return matches == 1u ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

static bool bounded_text(const char *value, size_t capacity, bool apn) {
    for (size_t i = 0u; i < capacity; ++i) {
        const unsigned char ch = (unsigned char)value[i];
        if (ch == 0u) return !apn || i != 0u;
        if (ch < 32u || ch > 126u ||
            (apn && (ch == '"' || ch == '\\'))) return false;
    }
    return false;
}

bool h2_lierda_apn_valid(const h2_pal_modem_apn_config_t *apn) {
    return apn != NULL && bounded_text(apn->apn, sizeof(apn->apn), true) &&
        bounded_text(apn->username, sizeof(apn->username), false) &&
        bounded_text(apn->password, sizeof(apn->password), false);
}

h2_pal_result_t h2_lierda_apply_apn(
    h2_lierda_modem_t *modem, const h2_pal_modem_apn_config_t *apn) {
    char command[H2_PAL_MODEM_APN_MAX + 32u];
    char response[H2_LIERDA_RESPONSE_SIZE];
    const int len = snprintf(command, sizeof(command),
        "AT+CGDCONT=1,\"IP\",\"%s\"", apn->apn);
    if (len < 0 || (size_t)len >= sizeof(command)) return H2_PAL_ERR_INVALID_ARG;
    return h2_lierda_command(modem, command, response);
}
