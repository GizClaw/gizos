/* Host test for the ESP Time provider: wall-clock validity must survive the
 * resets that keep the RTC timer (deep sleep, software reset) and must be
 * dropped by the resets that do not (power-on, brownout). */
#include "h2_esp_platform_core.h"

#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

/* 2024-05-06T07:08:09Z, comfortably after the retained-clock floor. */
#define TEST_WALL_MS 1714979289000ull
/* Eleven minutes of deep sleep while charging. */
#define TEST_SLEEP_MS 660000ull

static struct timeval s_clock;
static esp_reset_reason_t s_reset_reason = ESP_RST_POWERON;

void h2_esp_platform_time_test_restart(void);
void h2_esp_platform_time_test_scramble_rtc(uint32_t value);

int h2_esp_platform_time_test_gettimeofday(struct timeval *tv) {
  *tv = s_clock;
  return 0;
}

int h2_esp_platform_time_test_settimeofday(const struct timeval *tv) {
  s_clock = *tv;
  return 0;
}

esp_reset_reason_t esp_reset_reason(void) { return s_reset_reason; }

int64_t esp_timer_get_time(void) { return 0; }

void vTaskDelay(TickType_t ticks) { (void)ticks; }

static void advance_clock_ms(uint64_t ms) {
  s_clock.tv_sec += (time_t)(ms / 1000u);
  s_clock.tv_usec += (suseconds_t)((ms % 1000u) * 1000u);
  if (s_clock.tv_usec >= 1000000) {
    s_clock.tv_sec += 1;
    s_clock.tv_usec -= 1000000;
  }
}

static uint64_t wall_ms(const h2_pal_time_api_t *api) {
  uint64_t value = 0u;
  (void)h2_pal_time_get_wall_ms(api, &value);
  return value;
}

static bool wall_valid(const h2_pal_time_api_t *api) {
  h2_pal_time_wall_status_t status = {0};
  assert(h2_pal_time_get_wall_status(api, &status) == H2_PAL_OK);
  return status.valid;
}

/* Cold boot with an uncalibrated RTC (seconds since boot) stays invalid even
 * when RTC memory happens to hold the marker pattern. */
static void test_power_on_boot_is_invalid(const h2_pal_time_api_t *api) {
  s_reset_reason = ESP_RST_POWERON;
  s_clock = (struct timeval){.tv_sec = 12, .tv_usec = 0};
  h2_esp_platform_time_test_scramble_rtc(0x57414c4cu);
  h2_esp_platform_time_test_restart();
  assert(!wall_valid(api));
  assert(wall_ms(api) == 0u);
}

/* Deep-sleep wake keeps the calibrated clock and its validity. */
static void test_deep_sleep_wake_keeps_clock(const h2_pal_time_api_t *api) {
  s_reset_reason = ESP_RST_POWERON;
  s_clock = (struct timeval){.tv_sec = 5, .tv_usec = 0};
  h2_esp_platform_time_test_scramble_rtc(0u);
  h2_esp_platform_time_test_restart();
  assert(!wall_valid(api));
  assert(h2_pal_time_set_wall_ms(api, TEST_WALL_MS) == H2_PAL_OK);
  assert(wall_valid(api));
  assert(wall_ms(api) == TEST_WALL_MS);

  s_reset_reason = ESP_RST_DEEPSLEEP;
  advance_clock_ms(TEST_SLEEP_MS);
  h2_esp_platform_time_test_restart();
  assert(wall_valid(api));
  assert(wall_ms(api) == TEST_WALL_MS + TEST_SLEEP_MS);
  h2_pal_time_wall_status_t status = {0};
  assert(h2_pal_time_get_wall_status(api, &status) == H2_PAL_OK);
  assert(status.source == H2_PAL_TIME_WALL_SOURCE_USER);
}

/* Software reset (self reboot, OTA handoff) also keeps it. */
static void test_software_reset_keeps_clock(const h2_pal_time_api_t *api) {
  s_reset_reason = ESP_RST_SW;
  advance_clock_ms(1000u);
  h2_esp_platform_time_test_restart();
  assert(wall_valid(api));
  assert(wall_ms(api) == TEST_WALL_MS + TEST_SLEEP_MS + 1000u);
}

/* A retained marker with an implausible clock is discarded. */
static void test_retained_marker_needs_plausible_clock(
    const h2_pal_time_api_t *api) {
  s_reset_reason = ESP_RST_DEEPSLEEP;
  s_clock = (struct timeval){.tv_sec = 30, .tv_usec = 0};
  h2_esp_platform_time_test_restart();
  assert(!wall_valid(api));
  /* The marker was dropped, so a later RTC-keeping reset stays invalid too. */
  s_clock = (struct timeval){.tv_sec = (time_t)(TEST_WALL_MS / 1000u), .tv_usec = 0};
  h2_esp_platform_time_test_restart();
  assert(!wall_valid(api));
}

/* Power-on and brownout resets drop the marker regardless of the clock. */
static void test_power_loss_drops_marker(const h2_pal_time_api_t *api) {
  s_reset_reason = ESP_RST_SW;
  s_clock = (struct timeval){.tv_sec = 5, .tv_usec = 0};
  h2_esp_platform_time_test_restart();
  assert(h2_pal_time_set_wall_ms(api, TEST_WALL_MS) == H2_PAL_OK);
  assert(wall_valid(api));

  s_reset_reason = ESP_RST_BROWNOUT;
  h2_esp_platform_time_test_restart();
  assert(!wall_valid(api));
  assert(wall_ms(api) == 0u);
  /* The next RTC-keeping reset must not resurrect the stale marker. */
  s_reset_reason = ESP_RST_SW;
  h2_esp_platform_time_test_restart();
  assert(!wall_valid(api));
}

/* Setting the clock after a probe overrides the probe conclusion. */
static void test_set_after_probe_marks_valid(const h2_pal_time_api_t *api) {
  s_reset_reason = ESP_RST_POWERON;
  s_clock = (struct timeval){.tv_sec = 5, .tv_usec = 0};
  h2_esp_platform_time_test_restart();
  assert(!wall_valid(api));
  assert(h2_pal_time_set_wall_ms(api, TEST_WALL_MS + 5000u) == H2_PAL_OK);
  assert(wall_valid(api));
  assert(wall_ms(api) == TEST_WALL_MS + 5000u);
  assert(h2_pal_time_set_wall_ms(api, 0u) == H2_PAL_ERR_INVALID_ARG);
  assert(wall_valid(api));
}

int main(void) {
  const h2_pal_time_api_t *api = h2_esp_platform_time_api();
  assert(api != NULL);
  test_power_on_boot_is_invalid(api);
  test_deep_sleep_wake_keeps_clock(api);
  test_software_reset_keeps_clock(api);
  test_retained_marker_needs_plausible_clock(api);
  test_power_loss_drops_marker(api);
  test_set_after_probe_marks_valid(api);
  return 0;
}
