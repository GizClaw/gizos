#include "h2_bm8563.h"
#include "h2_bm8563_fake.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static h2_bm8563_t make_rtc(h2_bm8563_fake_t *fake) {
    h2_bm8563_t rtc;
    h2_bm8563_config_t config = {
        .transport = h2_bm8563_fake_transport(fake),
        .century_base_year = 1900u,
    };

    assert(h2_bm8563_init(&rtc, &config) == H2_PAL_OK);
    return rtc;
}

static void load_2024_leap_day(h2_bm8563_fake_t *fake) {
    static const uint8_t time_registers[] = {
        0x56u,
        0x34u,
        0x12u,
        0x29u,
        0x04u,
        0x02u,
        0x24u,
    };

    memcpy(&fake->registers[H2_BM8563_REGISTER_VL_SECONDS],
           time_registers,
           sizeof(time_registers));
}

static void test_init_validation(void) {
    h2_bm8563_t rtc;
    h2_bm8563_fake_t fake;
    h2_bm8563_config_t config;

    h2_bm8563_fake_init(&fake);
    config = (h2_bm8563_config_t){
        .transport = h2_bm8563_fake_transport(&fake),
        .century_base_year = 1900u,
    };
    assert(h2_bm8563_init(NULL, &config) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_bm8563_init(&rtc, NULL) == H2_PAL_ERR_INVALID_ARG);
    config.century_base_year = 1950u;
    assert(h2_bm8563_init(&rtc, &config) == H2_PAL_ERR_INVALID_ARG);
    config.century_base_year = 1900u;
    config.transport.vtable = NULL;
    assert(h2_bm8563_init(&rtc, &config) == H2_PAL_ERR_INVALID_ARG);
}

static void test_read_coherent_calendar(void) {
    h2_bm8563_fake_t fake;
    h2_bm8563_calendar_t calendar;
    h2_bm8563_t rtc;

    h2_bm8563_fake_init(&fake);
    load_2024_leap_day(&fake);
    rtc = make_rtc(&fake);
    assert(h2_bm8563_read_calendar(&rtc, &calendar) == H2_PAL_OK);
    assert(calendar.year == 2024u);
    assert(calendar.month == 2u);
    assert(calendar.day == 29u);
    assert(calendar.hour == 12u);
    assert(calendar.minute == 34u);
    assert(calendar.second == 56u);
    assert(calendar.weekday == 4u);
    assert(fake.operation_count == 1u);
    assert(fake.operations[0].kind == H2_BM8563_FAKE_OPERATION_READ);
    assert(fake.operations[0].start_register == H2_BM8563_REGISTER_VL_SECONDS);
    assert(fake.operations[0].data_length == H2_BM8563_TIME_REGISTER_COUNT);
}

static void test_invalid_rtc_data(void) {
    h2_bm8563_fake_t fake;
    h2_bm8563_calendar_t calendar;
    h2_bm8563_t rtc;

    h2_bm8563_fake_init(&fake);
    load_2024_leap_day(&fake);
    rtc = make_rtc(&fake);
    fake.registers[H2_BM8563_REGISTER_VL_SECONDS] |= 0x80u;
    assert(h2_bm8563_read_calendar(&rtc, &calendar) ==
           H2_PAL_ERR_INVALID_STATE);

    load_2024_leap_day(&fake);
    fake.registers[H2_BM8563_REGISTER_VL_SECONDS + 3u] = 0x30u;
    assert(h2_bm8563_read_calendar(&rtc, &calendar) ==
           H2_PAL_ERR_INVALID_STATE);

    load_2024_leap_day(&fake);
    fake.registers[H2_BM8563_REGISTER_VL_SECONDS + 1u] = 0x6au;
    assert(h2_bm8563_read_calendar(&rtc, &calendar) ==
           H2_PAL_ERR_INVALID_STATE);

    load_2024_leap_day(&fake);
    fake.registers[H2_BM8563_REGISTER_VL_SECONDS + 4u] = 0x03u;
    assert(h2_bm8563_read_calendar(&rtc, &calendar) ==
           H2_PAL_ERR_INVALID_STATE);

    load_2024_leap_day(&fake);
    fake.registers[H2_BM8563_REGISTER_VL_SECONDS + 2u] |= 0x40u;
    assert(h2_bm8563_read_calendar(&rtc, &calendar) ==
           H2_PAL_ERR_INVALID_STATE);
}

static void test_unix_round_trips(void) {
    h2_bm8563_calendar_t calendar;
    uint64_t unix_ms;

    assert(h2_bm8563_unix_ms_to_calendar(0u, &calendar) == H2_PAL_OK);
    assert(calendar.year == 1970u && calendar.month == 1u &&
           calendar.day == 1u && calendar.weekday == 4u);
    assert(h2_bm8563_calendar_to_unix_ms(&calendar, &unix_ms) == H2_PAL_OK);
    assert(unix_ms == 0u);

    assert(h2_bm8563_unix_ms_to_calendar(UINT64_C(951782400000), &calendar) ==
           H2_PAL_OK);
    assert(calendar.year == 2000u && calendar.month == 2u &&
           calendar.day == 29u && calendar.weekday == 2u);
    assert(h2_bm8563_calendar_to_unix_ms(&calendar, &unix_ms) == H2_PAL_OK);
    assert(unix_ms == UINT64_C(951782400000));

    assert(h2_bm8563_unix_ms_to_calendar(UINT64_C(4102444799000), &calendar) ==
           H2_PAL_OK);
    assert(calendar.year == 2099u && calendar.month == 12u &&
           calendar.day == 31u);
    assert(h2_bm8563_unix_ms_to_calendar(UINT64_C(4102444800000), &calendar) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_bm8563_unix_ms_to_calendar(1u, &calendar) ==
           H2_PAL_ERR_INVALID_ARG);
}

static void test_write_and_readback(void) {
    h2_bm8563_fake_t fake;
    h2_bm8563_calendar_t calendar = {
        .year = 2099u,
        .month = 12u,
        .day = 31u,
        .hour = 23u,
        .minute = 59u,
        .second = 59u,
        .weekday = 4u,
    };
    h2_bm8563_t rtc;

    h2_bm8563_fake_init(&fake);
    load_2024_leap_day(&fake);
    fake.registers[H2_BM8563_REGISTER_CONTROL_STATUS_1] = 0x08u;
    rtc = make_rtc(&fake);
    assert(h2_bm8563_write_calendar(&rtc, &calendar) == H2_PAL_OK);
    assert(fake.registers[H2_BM8563_REGISTER_CONTROL_STATUS_1] == 0x08u);
    assert(fake.registers[H2_BM8563_REGISTER_VL_SECONDS] == 0x59u);
    assert(fake.registers[H2_BM8563_REGISTER_VL_SECONDS + 5u] == 0x12u);
    assert(fake.registers[H2_BM8563_REGISTER_VL_SECONDS + 6u] == 0x99u);
    assert(fake.operation_count == 6u);
    assert(fake.operations[3].kind == H2_BM8563_FAKE_OPERATION_WRITE);
    assert(fake.operations[3].data_length == H2_BM8563_TIME_REGISTER_COUNT);
}

static void test_century_bit_selects_1900s(void) {
    h2_bm8563_fake_t fake;
    h2_bm8563_calendar_t calendar;
    h2_bm8563_t rtc;

    h2_bm8563_fake_init(&fake);
    load_2024_leap_day(&fake);
    fake.registers[H2_BM8563_REGISTER_VL_SECONDS + 3u] = 0x28u;
    fake.registers[H2_BM8563_REGISTER_VL_SECONDS + 4u] = 0x00u;
    fake.registers[H2_BM8563_REGISTER_VL_SECONDS + 5u] = 0x82u;
    fake.registers[H2_BM8563_REGISTER_VL_SECONDS + 6u] = 0x99u;
    rtc = make_rtc(&fake);

    assert(h2_bm8563_read_calendar(&rtc, &calendar) == H2_PAL_OK);
    assert(calendar.year == 1999u);
    assert(calendar.month == 2u);
    assert(calendar.day == 28u);

    assert(h2_bm8563_write_calendar(&rtc, &calendar) == H2_PAL_OK);
    assert(fake.registers[H2_BM8563_REGISTER_VL_SECONDS + 5u] == 0x82u);
    assert(fake.registers[H2_BM8563_REGISTER_VL_SECONDS + 6u] == 0x99u);
}

static void test_set_truncates_subsecond_precision(void) {
    h2_bm8563_fake_t fake;
    h2_bm8563_t rtc;

    h2_bm8563_fake_init(&fake);
    load_2024_leap_day(&fake);
    rtc = make_rtc(&fake);
    assert(h2_bm8563_set_unix_ms(&rtc, UINT64_C(1735689600999)) == H2_PAL_OK);
    assert(fake.registers[H2_BM8563_REGISTER_VL_SECONDS] == 0x00u);
    assert(fake.registers[H2_BM8563_REGISTER_VL_SECONDS + 1u] == 0x00u);
    assert(fake.registers[H2_BM8563_REGISTER_VL_SECONDS + 2u] == 0x00u);
}

static void test_invalid_write_does_not_touch_transport(void) {
    h2_bm8563_fake_t fake;
    h2_bm8563_calendar_t calendar = {
        .year = 2023u,
        .month = 2u,
        .day = 29u,
        .weekday = 3u,
    };
    h2_bm8563_t rtc;

    h2_bm8563_fake_init(&fake);
    load_2024_leap_day(&fake);
    rtc = make_rtc(&fake);
    assert(h2_bm8563_write_calendar(&rtc, &calendar) == H2_PAL_ERR_INVALID_ARG);
    assert(fake.operation_count == 0u);
}

static void test_failed_write_restores_previous_registers(void) {
    h2_bm8563_fake_t fake;
    h2_bm8563_calendar_t calendar = {
        .year = 2025u,
        .month = 1u,
        .day = 1u,
        .weekday = 3u,
    };
    uint8_t previous_registers[H2_BM8563_TIME_REGISTER_COUNT];
    h2_bm8563_t rtc;

    h2_bm8563_fake_init(&fake);
    load_2024_leap_day(&fake);
    memcpy(previous_registers,
           &fake.registers[H2_BM8563_REGISTER_VL_SECONDS],
           sizeof(previous_registers));
    fake.registers[H2_BM8563_REGISTER_CONTROL_STATUS_1] = 0x08u;
    rtc = make_rtc(&fake);
    h2_bm8563_fake_fail_operation(&fake, 4u, H2_PAL_ERR_IO);
    assert(h2_bm8563_write_calendar(&rtc, &calendar) == H2_PAL_ERR_IO);
    assert(fake.registers[H2_BM8563_REGISTER_CONTROL_STATUS_1] == 0x08u);
    assert(memcmp(&fake.registers[H2_BM8563_REGISTER_VL_SECONDS],
                  previous_registers,
                  sizeof(previous_registers)) == 0);
}

static void test_transport_failure_is_propagated(void) {
    h2_bm8563_fake_t fake;
    h2_bm8563_calendar_t calendar;
    h2_bm8563_t rtc;

    h2_bm8563_fake_init(&fake);
    load_2024_leap_day(&fake);
    rtc = make_rtc(&fake);
    h2_bm8563_fake_fail_operation(&fake, 1u, H2_PAL_ERR_TIMEOUT);
    assert(h2_bm8563_read_calendar(&rtc, &calendar) == H2_PAL_ERR_TIMEOUT);
}

int main(void) {
    test_init_validation();
    test_read_coherent_calendar();
    test_invalid_rtc_data();
    test_unix_round_trips();
    test_write_and_readback();
    test_century_bit_selects_1900s();
    test_set_truncates_subsecond_precision();
    test_invalid_write_does_not_touch_transport();
    test_failed_write_restores_previous_registers();
    test_transport_failure_is_propagated();
    return 0;
}
