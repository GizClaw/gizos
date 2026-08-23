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
