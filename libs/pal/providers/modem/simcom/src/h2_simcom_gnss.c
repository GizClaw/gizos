#include "h2_simcom_internal.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_coord_e7(
    const char *value,
    char hemi,
    int max_degrees,
    int32_t *out) {
    if (value == NULL || out == NULL) {
        return 0;
    }
    char *end = NULL;
    double raw = strtod(value, &end);
    if (end == value || *end != '\0' || !isfinite(raw) || raw < 0.0) {
        return 0;
    }
    int degrees = (int)(raw / 100.0);
    double minutes = raw - (double)(degrees * 100);
    if (degrees < 0 || degrees > max_degrees || minutes < 0.0 ||
        minutes >= 60.0 || (degrees == max_degrees && minutes != 0.0)) {
        return 0;
    }
    double decimal = (double)degrees + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W') {
        decimal = -decimal;
    }
    *out = (int32_t)(decimal * 10000000.0);
    return 1;
}

static int parse_fix_datetime(
    const char *time_buf,
    const char *date_buf,
    h2_pal_modem_gnss_fix_t *out_fix) {
    unsigned hour = 0u;
    unsigned minute = 0u;
    unsigned second = 0u;
    unsigned day = 0u;
    unsigned month = 0u;
    unsigned year = 0u;
    if (time_buf == NULL || date_buf == NULL ||
        sscanf(time_buf, "%2u%2u%2u", &hour, &minute, &second) != 3 ||
        sscanf(date_buf, "%2u%2u%2u", &day, &month, &year) != 3 ||
        hour > 23u || minute > 59u || second > 60u ||
        day == 0u || day > 31u || month == 0u || month > 12u) {
        return 0;
    }
    out_fix->hour = (uint8_t)hour;
    out_fix->minute = (uint8_t)minute;
    out_fix->second = (uint8_t)second;
    out_fix->day = (uint8_t)day;
    out_fix->month = (uint8_t)month;
    out_fix->year = (uint16_t)(year >= 80u ? 1900u + year : 2000u + year);
    return 1;
}

h2_pal_result_t h2_simcom_modem_gnss_start(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    modem->gnss_ready_observed = 0u;
    h2_simcom_response_t response;
    h2_pal_result_t rc = h2_simcom_at_exchange(
        modem, "AT+CGNSSPWR=1", &response, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (modem->config.wait_gnss_ready != NULL) {
        return modem->config.wait_gnss_ready(
            modem->config.transport_user, timeout_ms);
    }
    return modem->gnss_ready_observed != 0u
        ? H2_PAL_OK
        : H2_PAL_ERR_UNSUPPORTED;
}

h2_pal_result_t h2_simcom_modem_gnss_stop(h2_pal_modem_t *platform, uint32_t timeout_ms) {
    (void)timeout_ms;
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
    if (modem == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = h2_simcom_at_exchange(
        modem, "AT+CGNSSPWR=0", NULL, 0);
    if (rc == H2_PAL_OK) {
        modem->gnss_ready_observed = 0u;
    }
    return rc;
}

h2_pal_result_t h2_simcom_modem_get_gnss_state(
    h2_pal_modem_t *platform,
    h2_pal_modem_gnss_state_t *out_state) {
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
    if (modem == NULL || out_state == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_simcom_response_t response;
    h2_pal_result_t rc = h2_simcom_at_exchange(modem, "AT+CGNSSPWR?", &response, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    int state = 0;
    const char *line = h2_simcom_response_find(&response, "+CGNSSPWR:");
    if (line == NULL || sscanf(line, "+CGNSSPWR: %d", &state) != 1) {
        return H2_PAL_ERR_FORMAT;
    }
    *out_state = state != 0 ? H2_PAL_MODEM_GNSS_ACQUIRING : H2_PAL_MODEM_GNSS_OFF;
    return H2_PAL_OK;
}

h2_pal_result_t h2_simcom_modem_get_gnss_fix(
    h2_pal_modem_t *platform,
    h2_pal_modem_gnss_fix_t *out_fix) {
    h2_simcom_modem_t *modem = h2_simcom_from_platform(platform);
    if (modem == NULL || out_fix == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_fix, 0, sizeof(*out_fix));
    h2_simcom_response_t response;
    h2_pal_result_t rc = h2_simcom_at_exchange(modem, "AT+CGPSINFO", &response, 0);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const char *line = h2_simcom_response_find(&response, "+CGPSINFO:");
    if (line == NULL || strstr(line, "+CGPSINFO: ,") != NULL) {
        return H2_PAL_ERR_UNAVAILABLE;
    }

    char time_buf[24] = {0};
    char lat_buf[24] = {0};
    char lon_buf[24] = {0};
    char date_buf[16] = {0};
    char lat_hemi = 'N';
    char lon_hemi = 'E';
    double altitude = 0.0;
    double cog = 0.0;
    double speed_knots = 0.0;
    int matched = sscanf(
        line,
        "+CGPSINFO: %23[0-9.],%c,%23[0-9.],%c,%15[^,],%23[^,],%lf,%lf,%lf",
        lat_buf,
        &lat_hemi,
        lon_buf,
        &lon_hemi,
        date_buf,
        time_buf,
        &altitude,
        &speed_knots,
        &cog);
    if (matched != 9) {
        return H2_PAL_ERR_FORMAT;
    }
    if ((lat_hemi != 'N' && lat_hemi != 'S') ||
        (lon_hemi != 'E' && lon_hemi != 'W') ||
        !parse_coord_e7(lat_buf, lat_hemi, 90, &out_fix->latitude_e7) ||
        !parse_coord_e7(lon_buf, lon_hemi, 180, &out_fix->longitude_e7) ||
        !isfinite(altitude) || !isfinite(speed_knots) || !isfinite(cog) ||
        altitude < (double)INT32_MIN / 100.0 ||
        altitude > (double)INT32_MAX / 100.0 ||
        speed_knots < 0.0 ||
        speed_knots > (double)INT32_MAX / 51.444444 ||
        cog < 0.0 || cog > 360.0 ||
        !parse_fix_datetime(time_buf, date_buf, out_fix)) {
        return H2_PAL_ERR_FORMAT;
    }
    out_fix->valid = 1u;
    out_fix->altitude_cm = (int32_t)(altitude * 100.0);
    out_fix->speed_cm_s = (int32_t)(speed_knots * 51.444444);
    out_fix->course_deg100 = (int32_t)(cog * 100.0);
    return H2_PAL_OK;
}
