#include "h2_bk3633_platform_core.h"

#include "h2_bk3633_power_sdk_fake.h"
#include "h2_bm8563_fake.h"

#include <assert.h>

static void test_monotonic_remains_independent(void) {
    const h2_pal_time_api_t *time = h2_bk3633_platform_time_api();
    uint64_t ms = 0u;
    uint64_t us = 0u;

    h2_bk3633_power_sdk_fake_set_time(32u, 0u);
    assert(h2_pal_time_get_monotonic_ms(time, &ms) == H2_PAL_OK);
    assert(ms == 10u);
    assert(h2_pal_time_get_monotonic_us(time, &us) == H2_PAL_OK);
    assert(us == 10000u);

    h2_bk3633_power_sdk_fake_set_time(31u, 0u);
    assert(h2_pal_time_get_monotonic_ms(time, &ms) == H2_PAL_OK);
    assert(ms == 10u);
    assert(h2_pal_time_get_monotonic_us(time, &us) == H2_PAL_OK);
    assert(us == 10000u);

    h2_bk3633_power_sdk_fake_set_time(32u, 3u);
    assert(h2_pal_time_get_monotonic_us(time, &us) == H2_PAL_OK);
    assert(us == 10001u);

    h2_bk3633_power_sdk_fake_set_time(64u, 0u);
    assert(h2_pal_time_get_monotonic_ms(time, &ms) == H2_PAL_OK);
    assert(ms == 20u);
}

static void test_wall_time_status_and_round_trip(void) {
    h2_bm8563_fake_t fake;
    h2_bk3633_platform_time_config_t config;
    h2_pal_time_wall_status_t status;
    const h2_pal_time_api_t *time = h2_bk3633_platform_time_api();
    uint64_t wall_ms = 0u;
    const uint64_t expected_ms = UINT64_C(1735689600) * 1000u;

    h2_bk3633_platform_time_deinit();
    assert(h2_pal_time_get_wall_status(time, &status) == H2_PAL_OK);
    assert(status.valid == 0u);
    assert(h2_pal_time_get_wall_ms(time, &wall_ms) == H2_PAL_ERR_INVALID_STATE);

    h2_bm8563_fake_init(&fake);
    config = (h2_bk3633_platform_time_config_t){
        .rtc =
            {
                .transport = h2_bm8563_fake_transport(&fake),
                .century_base_year = 1900u,
            },
    };
    assert(h2_bk3633_platform_time_init(&config) == H2_PAL_OK);
    assert(h2_pal_time_get_wall_status(time, &status) == H2_PAL_OK);
    assert(status.valid == 0u);

    assert(h2_pal_time_set_wall_ms(time, expected_ms) == H2_PAL_OK);
    assert(h2_pal_time_get_wall_ms(time, &wall_ms) == H2_PAL_OK);
    assert(wall_ms == expected_ms);
    assert(h2_pal_time_get_wall_status(time, &status) == H2_PAL_OK);
    assert(status.valid == 1u);
    assert(status.source == H2_PAL_TIME_WALL_SOURCE_RTC);

    h2_bm8563_fake_fail_operation(
        &fake, fake.operation_count + 1u, H2_PAL_ERR_IO);
    assert(h2_pal_time_get_wall_ms(time, &wall_ms) == H2_PAL_ERR_IO);
    h2_bm8563_fake_fail_operation(
        &fake, fake.operation_count + 1u, H2_PAL_ERR_IO);
    assert(h2_pal_time_get_wall_status(time, &status) == H2_PAL_OK);
    assert(status.valid == 0u);
    h2_bk3633_platform_time_deinit();
}

int main(void) {
    test_monotonic_remains_independent();
    test_wall_time_status_and_round_trip();
    return 0;
}
