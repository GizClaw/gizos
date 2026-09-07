/* Host test for the ESP Time provider: wall-clock validity is derived from the
 * clock reading, so it survives the resets that keep the RTC timer running and
 * is withheld when the RTC restarts near the epoch. */
#include "h2_esp_platform_core.h"

#include "esp_timer.h"
#include "freertos/task.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>

/* 2024-05-06T07:08:09Z, comfortably after the plausibility floor. */
#define TEST_WALL_MS 1714979289000ull
/* 2019-12-31T23:59:59Z, one second before the floor. */
#define TEST_BEFORE_FLOOR_MS 1577836799000ull
/* Eleven minutes of deep sleep while charging. */
#define TEST_SLEEP_MS 660000ull

static struct timeval s_clock;
static int s_clock_read_fails;

void h2_esp_platform_time_test_restart(void);

int h2_esp_platform_time_test_gettimeofday(struct timeval *tv) {
  if (s_clock_read_fails) {
    return -1;
  }
  *tv = s_clock;
  return 0;
}

int h2_esp_platform_time_test_settimeofday(const struct timeval *tv) {
  s_clock = *tv;
  return 0;
}

int64_t esp_timer_get_time(void) { return 0; }

void vTaskDelay(TickType_t ticks) { (void)ticks; }

static void set_clock_ms(uint64_t ms) {
  s_clock.tv_sec = (time_t)(ms / 1000u);
  s_clock.tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
}

static void advance_clock_ms(uint64_t ms) {
  set_clock_ms(((uint64_t)s_clock.tv_sec * 1000u) +
               ((uint64_t)s_clock.tv_usec / 1000u) + ms);
}

static uint64_t wall_ms(const h2_pal_time_api_t *api) {
  uint64_t value = 0u;
  (void)h2_pal_time_get_wall_ms(api, &value);
  return value;
}

static h2_pal_time_wall_status_t wall_status(const h2_pal_time_api_t *api) {
  h2_pal_time_wall_status_t status = {0};
  assert(h2_pal_time_get_wall_status(api, &status) == H2_PAL_OK);
  return status;
}

/* A cold boot leaves the clock near the epoch: uncalibrated, no time shown. */
static void test_epoch_clock_is_invalid(const h2_pal_time_api_t *api) {
  set_clock_ms(12000u);
  h2_esp_platform_time_test_restart();
  const h2_pal_time_wall_status_t status = wall_status(api);
  assert(!status.valid);
  assert(status.source == H2_PAL_TIME_WALL_SOURCE_BOOT_DEFAULT);
  assert(wall_ms(api) == 0u);
}

/* Setting the clock marks it calibrated by this session. */
static void test_set_marks_user_source(const h2_pal_time_api_t *api) {
  assert(h2_pal_time_set_wall_ms(api, TEST_WALL_MS) == H2_PAL_OK);
  const h2_pal_time_wall_status_t status = wall_status(api);
  assert(status.valid);
  assert(status.source == H2_PAL_TIME_WALL_SOURCE_USER);
  assert(wall_ms(api) == TEST_WALL_MS);
}

/* Deep-sleep wake and software reset keep the RTC-backed clock, so the time
 * stays available without another calibration; the source becomes RTC. */
static void test_restart_keeps_clock(const h2_pal_time_api_t *api) {
  advance_clock_ms(TEST_SLEEP_MS);
  h2_esp_platform_time_test_restart();
  const h2_pal_time_wall_status_t status = wall_status(api);
  assert(status.valid);
  assert(status.source == H2_PAL_TIME_WALL_SOURCE_RTC);
  assert(wall_ms(api) == TEST_WALL_MS + TEST_SLEEP_MS);
}

/* A reading just before the floor is treated as uncalibrated. */
static void test_reading_before_floor_is_invalid(const h2_pal_time_api_t *api) {
  set_clock_ms(TEST_BEFORE_FLOOR_MS);
  h2_esp_platform_time_test_restart();
  assert(!wall_status(api).valid);
  assert(wall_ms(api) == 0u);
}

/* Losing the RTC (power-on, brownout) drops validity even after a session that
 * had set the clock. */
static void test_clock_loss_drops_validity(const h2_pal_time_api_t *api) {
  set_clock_ms(5000u);
  h2_esp_platform_time_test_restart();
  assert(h2_pal_time_set_wall_ms(api, TEST_WALL_MS) == H2_PAL_OK);
  assert(wall_status(api).valid);
  set_clock_ms(5000u);
  h2_esp_platform_time_test_restart();
  assert(!wall_status(api).valid);
  assert(wall_ms(api) == 0u);
}

/* A rejected set leaves the previous calibration untouched. */
static void test_rejected_set_keeps_state(const h2_pal_time_api_t *api) {
  set_clock_ms(5000u);
  h2_esp_platform_time_test_restart();
  assert(h2_pal_time_set_wall_ms(api, TEST_WALL_MS) == H2_PAL_OK);
  assert(h2_pal_time_set_wall_ms(api, 0u) == H2_PAL_ERR_INVALID_ARG);
  const h2_pal_time_wall_status_t status = wall_status(api);
  assert(status.valid);
  assert(status.source == H2_PAL_TIME_WALL_SOURCE_USER);
  assert(wall_ms(api) == TEST_WALL_MS);
}

/* An unreadable clock is an error, not an uncalibrated clock. */
static void test_read_failure_is_an_error(const h2_pal_time_api_t *api) {
  set_clock_ms(TEST_WALL_MS);
  h2_esp_platform_time_test_restart();
  s_clock_read_fails = 1;
  h2_pal_time_wall_status_t status = {0};
  assert(h2_pal_time_get_wall_status(api, &status) == H2_PAL_ERR_UNAVAILABLE);
  uint64_t value = 1u;
  assert(h2_pal_time_get_wall_ms(api, &value) == H2_PAL_ERR_UNAVAILABLE);
  assert(value == 0u);
  s_clock_read_fails = 0;
}

/* A pre-epoch reading must not wrap past the plausibility floor. */
static void test_pre_epoch_clock_is_unavailable(const h2_pal_time_api_t *api) {
  h2_esp_platform_time_test_restart();
  s_clock.tv_sec = -1;
  s_clock.tv_usec = 0;
  h2_pal_time_wall_status_t status = {0};
  assert(h2_pal_time_get_wall_status(api, &status) == H2_PAL_ERR_UNAVAILABLE);
  assert(wall_ms(api) == 0u);
  s_clock.tv_sec = 0;
  s_clock.tv_usec = 1000000;
  assert(h2_pal_time_get_wall_status(api, &status) == H2_PAL_ERR_UNAVAILABLE);
}

int main(void) {
  const h2_pal_time_api_t *api = h2_esp_platform_time_api();
  assert(api != NULL);
  test_epoch_clock_is_invalid(api);
  test_set_marks_user_source(api);
  test_restart_keeps_clock(api);
  test_reading_before_floor_is_invalid(api);
  test_clock_loss_drops_validity(api);
  test_rejected_set_keeps_state(api);
  test_read_failure_is_an_error(api);
  test_pre_epoch_clock_is_unavailable(api);
  return 0;
}
