#include "h2_quectel_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_coord_e7(const char *value, char hemi, int32_t *out) {
    if (value == NULL || out == NULL) {
        return 0;
    }
    double raw = strtod(value, NULL);
    int degrees = (int)(raw / 100.0);
    double minutes = raw - (double)(degrees * 100);
    double decimal = (double)degrees + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W') {
        decimal = -decimal;
    }
    *out = (int32_t)(decimal * 10000000.0);
    return 1;
}

static void parse_fix_datetime(const char *time_buf, const char *date_buf, h2_pal_modem_gnss_fix_t *out_fix) {
    unsigned hour = 0u;
    unsigned minute = 0u;
    unsigned second = 0u;
    unsigned day = 0u;
    unsigned month = 0u;
    unsigned year = 0u;
    if (time_buf != NULL && sscanf(time_buf, "%2u%2u%2u", &hour, &minute, &second) == 3) {
        out_fix->hour = (uint8_t)hour;
        out_fix->minute = (uint8_t)minute;
        out_fix->second = (uint8_t)second;
    }
    if (date_buf != NULL && sscanf(date_buf, "%2u%2u%2u", &day, &month, &year) == 3) {
        out_fix->day = (uint8_t)day;
        out_fix->month = (uint8_t)month;
        out_fix->year = (uint16_t)(year >= 80u ? 1900u + year : 2000u + year);
    }
}

h2_pal_result_t h2_quectel_modem_gnss_start(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    (void)timeout_ms;
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    return modem != NULL ? h2_quectel_at_exchange(modem, "AT+QGPS=1", NULL, 0) : H2_PAL_ERR_INVALID_ARG;
}

h2_pal_result_t h2_quectel_modem_gnss_stop(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    (void)timeout_ms;
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    return modem != NULL ? h2_quectel_at_exchange(modem, "AT+QGPSEND", NULL, 0) : H2_PAL_ERR_INVALID_ARG;
}

h2_pal_result_t h2_quectel_modem_get_gnss_state(
    h2_pal_modem_t *platform,
    h2_pal_modem_gnss_state_t *out_state) {
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL || out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_quectel_response_t response;
    h2_pal_result_t rc = h2_quectel_at_exchange(modem, "AT+QGPS?", &response, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    int state = 0;
    if (!h2_quectel_parse_int_after(h2_quectel_response_find(&response, "+QGPS:"), "+QGPS:", &state)) {
        return H2_PAL_ERR_FORMAT;
    }
    *out_state = state != 0 ? H2_PAL_MODEM_GNSS_ACQUIRING : H2_PAL_MODEM_GNSS_OFF;
    return H2_PAL_OK;
}

h2_pal_result_t h2_quectel_modem_get_gnss_fix(
    h2_pal_modem_t *platform,
    h2_pal_modem_gnss_fix_t *out_fix) {
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL || out_fix == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_fix, 0, sizeof(*out_fix));
    h2_quectel_response_t response;
    h2_pal_result_t rc = h2_quectel_at_exchange(modem, "AT+QGPSLOC=0", &response, 0);
    if (rc != H2_PAL_OK) {
        if (h2_quectel_response_find(&response, "+CME ERROR: 516") != NULL) {
            return H2_PAL_ERR_UNAVAILABLE;
        }
        return rc;
    }
    const char *line = h2_quectel_response_find(&response, "+QGPSLOC:");
    if (line == NULL) {
        return H2_PAL_ERR_UNAVAILABLE;
    }

    char time_buf[24] = {0};
    char lat_buf[24] = {0};
    char lon_buf[24] = {0};
    char date_buf[16] = {0};
    char lat_hemi = 'N';
    char lon_hemi = 'E';
    double hdop = 0.0;
    double altitude = 0.0;
    double cog = 0.0;
    double speed_kmh = 0.0;
    int fix_quality = 0;
    int sats = 0;
    int matched = sscanf(
        line,
        "+QGPSLOC: %23[^,],%23[0-9.]%c,%23[0-9.]%c,%lf,%lf,%d,%lf,%lf,%*[^,],%15[^,],%d",
        time_buf,
        lat_buf,
        &lat_hemi,
        lon_buf,
        &lon_hemi,
        &hdop,
        &altitude,
        &fix_quality,
        &cog,
        &speed_kmh,
        date_buf,
        &sats);
    if (matched < 12) {
        return H2_PAL_ERR_FORMAT;
    }
    out_fix->valid = fix_quality > 0 ? 1u : 0u;
    (void)parse_coord_e7(lat_buf, lat_hemi, &out_fix->latitude_e7);
    (void)parse_coord_e7(lon_buf, lon_hemi, &out_fix->longitude_e7);
    out_fix->altitude_cm = (int32_t)(altitude * 100.0);
    out_fix->speed_cm_s = (int32_t)(speed_kmh * 100000.0 / 3600.0);
    out_fix->course_deg100 = (int32_t)(cog * 100.0);
    out_fix->hdop100 = (uint16_t)(hdop * 100.0);
    out_fix->satellites = (uint8_t)sats;
    parse_fix_datetime(time_buf, date_buf, out_fix);
    return H2_PAL_OK;
}

/* QuecLocator, the operator-side cell location service. It is reached over
 * packet data and authenticated with a token that identifies the integrator,
 * so the token is never copied into modem state, a response buffer, an error
 * message or any log line: only the QLBSCFG command that has to carry it ever
 * sees the value. */

#define QUECTEL_QLBS_PREFIX "+QLBS:"
#define QUECTEL_CELL_LOCATE_LAT_LIMIT_E7 900000000
#define QUECTEL_CELL_LOCATE_LON_LIMIT_E7 1800000000

int h2_quectel_cell_locate_token_valid(const char *token) {
    if (token == NULL || token[0] == '\0') {
        return 0;
    }
    size_t len = 0u;
    for (const char *cursor = token; *cursor != '\0'; ++cursor) {
        /* These would break the AT command framing and let a malformed token
         * turn into extra commands. */
        if (*cursor == '"' || *cursor == ',' || *cursor == '\r' || *cursor == '\n') {
            return 0;
        }
        len++;
        if (len > H2_QUECTEL_CELL_LOCATE_TOKEN_MAX) {
            return 0;
        }
    }
    return 1;
}

/* QuecLocator error codes, from the QuecLocator application note. */
static h2_pal_result_t map_qlbs_result(long code) {
    switch (code) {
        case 10000: /* Positioning failed. */
            return H2_PAL_ERR_UNAVAILABLE;
        case 10001: /* IMEI is illegal. */
        case 10007: /* IMEI is not accepted by the server. */
            return H2_PAL_ERR_INVALID_STATE;
        case 10002: /* The token does not exist. */
        case 10006: /* The token is expired. */
            return H2_PAL_ERR_INVALID_ARG;
        case 10003: /* Devices per token exceeded. */
        case 10004: /* Daily requests per device exceeded. */
        case 10005: /* Total requests per token exceeded. */
        case 10008: /* Daily requests per token exceeded. */
        case 10009: /* Request frequency per token exceeded. */
            return H2_PAL_ERR_BUSY;
        case 702: /* The server did not answer within the configured time. */
            return H2_PAL_ERR_TIMEOUT;
        default:
            return H2_PAL_ERR_IO;
    }
}

/* AT+CMEE=2 is configured in prepare(), so ME errors arrive as verbose text;
 * numeric codes are still accepted for modems left in AT+CMEE=1. */
static h2_pal_result_t map_cell_locate_cme_error(
    const h2_quectel_response_t *response,
    h2_pal_result_t fallback) {
    const char *line = h2_quectel_response_find(response, "+CME ERROR:");
    if (line == NULL) {
        return fallback;
    }
    int code = 0;
    if (h2_quectel_parse_int_after(line, "+CME ERROR:", &code)) {
        switch (code) {
            case 3: /* Operation not allowed: the PDP context is not activated. */
                return H2_PAL_ERR_INVALID_STATE;
            case 4:
                return H2_PAL_ERR_UNSUPPORTED;
            case 516: /* QuecLocator rejected the token. */
                return H2_PAL_ERR_INVALID_ARG;
            default:
                return fallback;
        }
    }
    if (strstr(line, "operation not allowed") != NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (strstr(line, "operation not supported") != NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return fallback;
}

static int response_contains_token(
    const h2_quectel_response_t *response,
    const char *token) {
    for (size_t i = 0u; i < response->count; ++i) {
        if (strstr(response->lines[i], token) != NULL) {
            return 1;
        }
    }
    return 0;
}

/* QuecLocator reports decimal degrees, unlike the NMEA degree-minute form of
 * AT+QGPSLOC. Returns the first character past the number, or NULL. */
static const char *parse_degrees_e7(const char *text, int32_t limit_e7, int32_t *out_value) {
    char *end = NULL;
    double degrees = strtod(text, &end);
    if (end == text) {
        return NULL;
    }
    double scaled = degrees * 10000000.0;
    double rounded = scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5;
    if (rounded > (double)limit_e7 || rounded < -(double)limit_e7) {
        return NULL;
    }
    *out_value = (int32_t)rounded;
    return end;
}

static h2_pal_result_t parse_qlbs_line(
    const char *line,
    h2_pal_modem_cell_location_t *out_location) {
    const char *cursor = line + strlen(QUECTEL_QLBS_PREFIX);
    while (h2_quectel_ascii_space((unsigned char)*cursor)) {
        cursor++;
    }
    char *end = NULL;
    long loc_result = strtol(cursor, &end, 10);
    if (end == cursor) {
        return H2_PAL_ERR_FORMAT;
    }
    if (loc_result != 0) {
        /* The failure form carries no coordinates, so nothing partial is
         * reported to the caller. */
        return map_qlbs_result(loc_result);
    }
    if (*end != ',') {
        return H2_PAL_ERR_FORMAT;
    }

    int32_t latitude_e7 = 0;
    int32_t longitude_e7 = 0;
    cursor = parse_degrees_e7(end + 1, QUECTEL_CELL_LOCATE_LAT_LIMIT_E7, &latitude_e7);
    if (cursor == NULL || *cursor != ',') {
        return H2_PAL_ERR_FORMAT;
    }
    cursor = parse_degrees_e7(cursor + 1, QUECTEL_CELL_LOCATE_LON_LIMIT_E7, &longitude_e7);
    if (cursor == NULL || (*cursor != ',' && *cursor != '\0')) {
        return H2_PAL_ERR_FORMAT;
    }

    uint32_t accuracy_m = 0u;
    if (*cursor == ',') {
        /* The optional field is an accuracy in metres on modems that report
         * one, and a quoted server timestamp on those that do not; only a bare
         * unsigned integer is taken as accuracy. */
        const char *tail = cursor + 1;
        unsigned long accuracy = strtoul(tail, &end, 10);
        if (end != tail && *end == '\0' && accuracy <= 0xFFFFFFFFuL) {
            accuracy_m = (uint32_t)accuracy;
        }
    }

    out_location->valid = 1u;
    out_location->latitude_e7 = latitude_e7;
    out_location->longitude_e7 = longitude_e7;
    out_location->accuracy_m = accuracy_m;
    return H2_PAL_OK;
}

static h2_pal_result_t cell_locate_send_token(h2_quectel_modem_t *modem, uint32_t timeout_ms) {
    const char *token = modem->config.cell_locate_token;
    char cmd[H2_QUECTEL_LINE_MAX];
    int n = snprintf(cmd, sizeof(cmd), "AT+QLBSCFG=\"token\",\"%s\"", token);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_quectel_response_t response;
    h2_pal_result_t rc = h2_quectel_at_exchange_timeout(modem, cmd, &response, 0, timeout_ms);
    /* The command echo is dropped by the exchange; a modem that still reports
     * the token back must not leave it in a buffer the caller can reach. */
    if (rc == H2_PAL_OK && response_contains_token(&response, token)) {
        rc = H2_PAL_ERR_IO;
    } else if (rc != H2_PAL_OK) {
        rc = map_cell_locate_cme_error(&response, rc);
    }
    memset(&response, 0, sizeof(response));
    memset(cmd, 0, sizeof(cmd));
    if (rc == H2_PAL_OK) {
        modem->cell_locate_token_sent = 1u;
    }
    return rc;
}

h2_pal_result_t h2_quectel_modem_cell_locate(
    h2_pal_modem_t *platform,
    uint32_t timeout_ms,
    h2_pal_modem_cell_location_t *out_location) {
    h2_quectel_modem_t *modem = h2_quectel_from_platform(platform);
    if (modem == NULL || out_location == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_location, 0, sizeof(*out_location));
    if ((h2_quectel_modem_capabilities(modem) & H2_PAL_MODEM_CAPABILITY_CELL_LOCATE) == 0u ||
        !h2_quectel_cell_locate_token_valid(modem->config.cell_locate_token)) {
        return H2_PAL_ERR_UNSUPPORTED;
    }

    uint32_t effective_timeout_ms = timeout_ms != 0u
        ? timeout_ms
        : modem->config.cell_locate_timeout_ms;
    if (effective_timeout_ms == 0u) {
        effective_timeout_ms = H2_QUECTEL_CELL_LOCATE_TIMEOUT_MS;
    }

    /* The token is pushed once per modem lifetime, so a product that never
     * calls cell locate never emits it. */
    if (modem->cell_locate_token_sent == 0u) {
        h2_pal_result_t rc = cell_locate_send_token(modem, effective_timeout_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }

    h2_quectel_response_t response;
    h2_pal_result_t rc = h2_quectel_at_exchange_timeout(modem, "AT+QLBS", &response, 0, effective_timeout_ms);
    const char *line = h2_quectel_response_find(&response, QUECTEL_QLBS_PREFIX);
    if (line == NULL) {
        return rc != H2_PAL_OK ? map_cell_locate_cme_error(&response, rc) : H2_PAL_ERR_FORMAT;
    }
    /* A failed positioning attempt reports "+QLBS: <loc_result>" and then
     * ERROR, so the service error code is more precise than the AT result. */
    h2_pal_result_t parsed = parse_qlbs_line(line, out_location);
    if (rc != H2_PAL_OK && parsed == H2_PAL_OK) {
        memset(out_location, 0, sizeof(*out_location));
        return rc;
    }
    return parsed;
}
