#include "h2_bm8563.h"

#include <stdbool.h>
#include <string.h>

#define BM8563_CONTROL_1_STOP (1u << 5)
#define BM8563_SECONDS_VL (1u << 7)
#define BM8563_MONTH_CENTURY_19XX (1u << 7)
#define BM8563_SECONDS_MASK 0x7fu
#define BM8563_MINUTES_MASK 0x7fu
#define BM8563_HOURS_MASK 0x3fu
#define BM8563_DAYS_MASK 0x3fu
#define BM8563_WEEKDAYS_MASK 0x07u
#define BM8563_MONTHS_MASK 0x1fu
#define BM8563_MILLISECONDS_PER_SECOND UINT64_C(1000)
#define BM8563_SECONDS_PER_DAY UINT64_C(86400)

static bool bm8563_is_leap_year(uint16_t year) {
    return (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
}

static uint8_t bm8563_days_in_month(uint16_t year, uint8_t month) {
    static const uint8_t days[] = {
        31u,
        28u,
        31u,
        30u,
        31u,
        30u,
        31u,
        31u,
        30u,
        31u,
        30u,
        31u,
    };

    if (month == 0u || month > 12u) {
        return 0u;
    }
    if (month == 2u && bm8563_is_leap_year(year)) {
        return 29u;
    }
    return days[month - 1u];
}

static h2_pal_result_t
bm8563_bcd_decode(uint8_t value, uint8_t maximum, uint8_t *out_value) {
    uint8_t high = (uint8_t)(value >> 4u);
    uint8_t low = (uint8_t)(value & 0x0fu);
    uint8_t decoded;

    if (out_value == NULL || high > 9u || low > 9u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    decoded = (uint8_t)(high * 10u + low);
    if (decoded > maximum) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_value = decoded;
    return H2_PAL_OK;
}

static uint8_t bm8563_bcd_encode(uint8_t value) {
    return (uint8_t)(((value / 10u) << 4u) | (value % 10u));
}

static h2_pal_result_t
bm8563_days_since_epoch(const h2_bm8563_calendar_t *calendar,
                        uint64_t *out_days) {
    uint64_t days = 0u;
    uint16_t year;
    uint8_t month;

    if (calendar == NULL || out_days == NULL || calendar->year < 1970u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (year = 1970u; year < calendar->year; ++year) {
        days += bm8563_is_leap_year(year) ? 366u : 365u;
    }
    for (month = 1u; month < calendar->month; ++month) {
        days += bm8563_days_in_month(calendar->year, month);
    }
    days += calendar->day - 1u;
    *out_days = days;
    return H2_PAL_OK;
}

static uint8_t bm8563_weekday_from_days(uint64_t days_since_epoch) {
    /* 1970-01-01 was Thursday; Bluetooth Monday is 1 and Sunday is 7. */
    return (uint8_t)(((days_since_epoch + 3u) % 7u) + 1u);
}

static h2_pal_result_t
bm8563_calendar_fields_validate(const h2_bm8563_calendar_t *calendar) {
    uint64_t days;
    uint8_t maximum_day;

    if (calendar == NULL || calendar->year < 1970u || calendar->year > 2099u ||
        calendar->month == 0u || calendar->month > 12u ||
        calendar->hour > 23u || calendar->minute > 59u ||
        calendar->second > 59u || calendar->weekday == 0u ||
        calendar->weekday > 7u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    maximum_day = bm8563_days_in_month(calendar->year, calendar->month);
    if (calendar->day == 0u || calendar->day > maximum_day) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (bm8563_days_since_epoch(calendar, &days) != H2_PAL_OK ||
        calendar->weekday != bm8563_weekday_from_days(days)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t bm8563_read_registers(h2_bm8563_t *rtc,
                                             uint8_t start_register,
                                             uint8_t *out_data,
                                             size_t data_length) {
    return rtc->transport.vtable->read_registers(
        rtc->transport.user, start_register, out_data, data_length);
}

static h2_pal_result_t bm8563_write_registers(h2_bm8563_t *rtc,
                                              uint8_t start_register,
                                              const uint8_t *data,
                                              size_t data_length) {
    return rtc->transport.vtable->write_registers(
        rtc->transport.user, start_register, data, data_length);
}

static h2_pal_result_t
bm8563_decode_calendar(const h2_bm8563_t *rtc,
                       const uint8_t registers[H2_BM8563_TIME_REGISTER_COUNT],
                       h2_bm8563_calendar_t *out_calendar) {
    h2_bm8563_calendar_t calendar = {0};
    uint8_t year_two_digits = 0u;
    uint8_t rtc_weekday;
    h2_pal_result_t rc;

    if ((registers[0] & BM8563_SECONDS_VL) != 0u ||
        (registers[1] & (uint8_t)(UINT8_MAX ^ BM8563_MINUTES_MASK)) != 0u ||
        (registers[2] & (uint8_t)(UINT8_MAX ^ BM8563_HOURS_MASK)) != 0u ||
        (registers[3] & (uint8_t)(UINT8_MAX ^ BM8563_DAYS_MASK)) != 0u ||
        (registers[4] & (uint8_t)(UINT8_MAX ^ BM8563_WEEKDAYS_MASK)) != 0u ||
        (registers[5] &
         (uint8_t)(UINT8_MAX ^
                   (BM8563_MONTH_CENTURY_19XX | BM8563_MONTHS_MASK))) != 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    rc = bm8563_bcd_decode(
        (uint8_t)(registers[0] & BM8563_SECONDS_MASK), 59u, &calendar.second);
    if (rc == H2_PAL_OK) {
        rc = bm8563_bcd_decode((uint8_t)(registers[1] & BM8563_MINUTES_MASK),
                               59u,
                               &calendar.minute);
    }
    if (rc == H2_PAL_OK) {
        rc = bm8563_bcd_decode(
            (uint8_t)(registers[2] & BM8563_HOURS_MASK), 23u, &calendar.hour);
    }
    if (rc == H2_PAL_OK) {
        rc = bm8563_bcd_decode(
            (uint8_t)(registers[3] & BM8563_DAYS_MASK), 31u, &calendar.day);
    }
    rtc_weekday = (uint8_t)(registers[4] & BM8563_WEEKDAYS_MASK);
    if (rc == H2_PAL_OK && rtc_weekday > 6u) {
        rc = H2_PAL_ERR_INVALID_ARG;
    }
    calendar.weekday = rtc_weekday == 0u ? 7u : rtc_weekday;
    if (rc == H2_PAL_OK) {
        rc = bm8563_bcd_decode(
            (uint8_t)(registers[5] & BM8563_MONTHS_MASK), 12u, &calendar.month);
    }
    if (rc == H2_PAL_OK) {
        rc = bm8563_bcd_decode(registers[6], 99u, &year_two_digits);
    }
    if (rc != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    calendar.year = (uint16_t)(rtc->century_base_year + year_two_digits);
    if ((registers[5] & BM8563_MONTH_CENTURY_19XX) == 0u) {
        calendar.year = (uint16_t)(calendar.year + 100u);
    }
    if (h2_bm8563_validate_calendar(rtc, &calendar) != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    *out_calendar = calendar;
    return H2_PAL_OK;
}

static h2_pal_result_t
bm8563_encode_calendar(const h2_bm8563_t *rtc,
                       const h2_bm8563_calendar_t *calendar,
                       uint8_t registers[H2_BM8563_TIME_REGISTER_COUNT]) {
    uint16_t year_offset;

    if (h2_bm8563_validate_calendar(rtc, calendar) != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    year_offset = (uint16_t)(calendar->year - rtc->century_base_year);
    registers[0] = bm8563_bcd_encode(calendar->second);
    registers[1] = bm8563_bcd_encode(calendar->minute);
    registers[2] = bm8563_bcd_encode(calendar->hour);
    registers[3] = bm8563_bcd_encode(calendar->day);
    registers[4] = calendar->weekday == 7u ? 0u : calendar->weekday;
    registers[5] = bm8563_bcd_encode(calendar->month);
    if (year_offset >= 100u) {
        year_offset = (uint16_t)(year_offset - 100u);
    } else {
        registers[5] |= BM8563_MONTH_CENTURY_19XX;
    }
    registers[6] = bm8563_bcd_encode((uint8_t)year_offset);
    return H2_PAL_OK;
}

static bool bm8563_calendars_equal(const h2_bm8563_calendar_t *left,
                                   const h2_bm8563_calendar_t *right) {
    return left->year == right->year && left->month == right->month &&
           left->day == right->day && left->hour == right->hour &&
           left->minute == right->minute && left->second == right->second &&
           left->weekday == right->weekday;
}

static void bm8563_restore_registers(
    h2_bm8563_t *rtc,
    uint8_t previous_control,
    const uint8_t previous_time[H2_BM8563_TIME_REGISTER_COUNT]) {
    uint8_t stopped_control =
        (uint8_t)(previous_control | BM8563_CONTROL_1_STOP);

    (void)bm8563_write_registers(
        rtc, H2_BM8563_REGISTER_CONTROL_STATUS_1, &stopped_control, 1u);
    (void)bm8563_write_registers(rtc,
                                 H2_BM8563_REGISTER_VL_SECONDS,
                                 previous_time,
                                 H2_BM8563_TIME_REGISTER_COUNT);
    (void)bm8563_write_registers(
        rtc, H2_BM8563_REGISTER_CONTROL_STATUS_1, &previous_control, 1u);
}

h2_pal_result_t h2_bm8563_init(h2_bm8563_t *rtc,
                               const h2_bm8563_config_t *config) {
    if (rtc == NULL || config == NULL || config->transport.vtable == NULL ||
        config->transport.vtable->read_registers == NULL ||
        config->transport.vtable->write_registers == NULL ||
        config->century_base_year < 1u ||
        config->century_base_year % 100u != 0u ||
        config->century_base_year > UINT16_MAX - 199u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *rtc = (h2_bm8563_t){
        .transport = config->transport,
        .century_base_year = config->century_base_year,
        .initialized = 1u,
    };
    return H2_PAL_OK;
}

void h2_bm8563_deinit(h2_bm8563_t *rtc) {
    if (rtc != NULL) {
        memset(rtc, 0, sizeof(*rtc));
    }
}

h2_pal_result_t
h2_bm8563_validate_calendar(const h2_bm8563_t *rtc,
                            const h2_bm8563_calendar_t *calendar) {
    uint32_t upper_year;

    if (rtc == NULL || rtc->initialized == 0u || calendar == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    upper_year = (uint32_t)rtc->century_base_year + 199u;
    if (calendar->year < rtc->century_base_year ||
        calendar->year > upper_year) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return bm8563_calendar_fields_validate(calendar);
}

h2_pal_result_t h2_bm8563_read_calendar(h2_bm8563_t *rtc,
                                        h2_bm8563_calendar_t *out_calendar) {
    uint8_t registers[H2_BM8563_TIME_REGISTER_COUNT];
    h2_pal_result_t rc;

    if (rtc == NULL || rtc->initialized == 0u || out_calendar == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_calendar, 0, sizeof(*out_calendar));
    rc = bm8563_read_registers(
        rtc, H2_BM8563_REGISTER_VL_SECONDS, registers, sizeof(registers));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return bm8563_decode_calendar(rtc, registers, out_calendar);
}

h2_pal_result_t h2_bm8563_write_calendar(h2_bm8563_t *rtc,
                                         const h2_bm8563_calendar_t *calendar) {
    uint8_t previous_control;
    uint8_t stopped_control;
    uint8_t running_control;
    uint8_t previous_time[H2_BM8563_TIME_REGISTER_COUNT];
    uint8_t encoded_time[H2_BM8563_TIME_REGISTER_COUNT];
    h2_bm8563_calendar_t readback;
    h2_pal_result_t rc;

    if (rtc == NULL || rtc->initialized == 0u || calendar == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = bm8563_encode_calendar(rtc, calendar, encoded_time);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = bm8563_read_registers(
        rtc, H2_BM8563_REGISTER_CONTROL_STATUS_1, &previous_control, 1u);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = bm8563_read_registers(rtc,
                               H2_BM8563_REGISTER_VL_SECONDS,
                               previous_time,
                               sizeof(previous_time));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    stopped_control = (uint8_t)(previous_control | BM8563_CONTROL_1_STOP);
    running_control =
        (uint8_t)(previous_control &
                  (BM8563_CONTROL_1_STOP ^ UINT8_MAX));
    rc = bm8563_write_registers(
        rtc, H2_BM8563_REGISTER_CONTROL_STATUS_1, &stopped_control, 1u);
    if (rc == H2_PAL_OK) {
        rc = bm8563_write_registers(rtc,
                                    H2_BM8563_REGISTER_VL_SECONDS,
                                    encoded_time,
                                    sizeof(encoded_time));
    }
    if (rc == H2_PAL_OK) {
        rc = bm8563_write_registers(
            rtc, H2_BM8563_REGISTER_CONTROL_STATUS_1, &running_control, 1u);
    }
    if (rc != H2_PAL_OK) {
        bm8563_restore_registers(rtc, previous_control, previous_time);
        return rc;
    }
    rc = h2_bm8563_read_calendar(rtc, &readback);
    if (rc != H2_PAL_OK || !bm8563_calendars_equal(&readback, calendar)) {
        bm8563_restore_registers(rtc, previous_control, previous_time);
        return rc != H2_PAL_OK ? rc : H2_PAL_ERR_IO;
    }
    return H2_PAL_OK;
}

h2_pal_result_t
h2_bm8563_calendar_to_unix_ms(const h2_bm8563_calendar_t *calendar,
                              uint64_t *out_unix_ms) {
    uint64_t days;
    uint64_t seconds;

    if (out_unix_ms == NULL ||
        bm8563_calendar_fields_validate(calendar) != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (bm8563_days_since_epoch(calendar, &days) != H2_PAL_OK) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    seconds = days * BM8563_SECONDS_PER_DAY;
    seconds += (uint64_t)calendar->hour * 3600u;
    seconds += (uint64_t)calendar->minute * 60u;
    seconds += calendar->second;
    *out_unix_ms = seconds * BM8563_MILLISECONDS_PER_SECOND;
    return H2_PAL_OK;
}

h2_pal_result_t
h2_bm8563_unix_ms_to_calendar(uint64_t unix_ms,
                              h2_bm8563_calendar_t *out_calendar) {
    h2_bm8563_calendar_t calendar = {0};
    uint64_t total_seconds;
    uint64_t days;
    uint64_t remaining;
    uint16_t year = 1970u;
    uint8_t month = 1u;

    if (out_calendar == NULL ||
        unix_ms % BM8563_MILLISECONDS_PER_SECOND != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    total_seconds = unix_ms / BM8563_MILLISECONDS_PER_SECOND;
    days = total_seconds / BM8563_SECONDS_PER_DAY;
    remaining = total_seconds % BM8563_SECONDS_PER_DAY;
    while (year <= 2099u) {
        uint16_t year_days = bm8563_is_leap_year(year) ? 366u : 365u;
        if (days < year_days) {
            break;
        }
        days -= year_days;
        ++year;
    }
    if (year > 2099u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    while (month <= 12u) {
        uint8_t month_days = bm8563_days_in_month(year, month);
        if (days < month_days) {
            break;
        }
        days -= month_days;
        ++month;
    }
    if (month > 12u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    calendar.year = year;
    calendar.month = month;
    calendar.day = (uint8_t)(days + 1u);
    calendar.hour = (uint8_t)(remaining / 3600u);
    remaining %= 3600u;
    calendar.minute = (uint8_t)(remaining / 60u);
    calendar.second = (uint8_t)(remaining % 60u);
    calendar.weekday =
        bm8563_weekday_from_days(total_seconds / BM8563_SECONDS_PER_DAY);
    *out_calendar = calendar;
    return H2_PAL_OK;
}

h2_pal_result_t h2_bm8563_get_unix_ms(h2_bm8563_t *rtc, uint64_t *out_unix_ms) {
    h2_bm8563_calendar_t calendar;
    h2_pal_result_t rc;

    if (out_unix_ms == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_bm8563_read_calendar(rtc, &calendar);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return h2_bm8563_calendar_to_unix_ms(&calendar, out_unix_ms);
}

h2_pal_result_t h2_bm8563_set_unix_ms(h2_bm8563_t *rtc, uint64_t unix_ms) {
    h2_bm8563_calendar_t calendar;
    uint64_t second_aligned_ms =
        unix_ms - unix_ms % BM8563_MILLISECONDS_PER_SECOND;
    h2_pal_result_t rc =
        h2_bm8563_unix_ms_to_calendar(second_aligned_ms, &calendar);

    if (rc != H2_PAL_OK) {
        return rc;
    }
    return h2_bm8563_write_calendar(rtc, &calendar);
}
