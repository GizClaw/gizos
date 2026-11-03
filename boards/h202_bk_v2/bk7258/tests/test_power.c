#include "h2_bk7258_board_private.h"
#include "h2_h202_board_test_sdk.h"
#include <assert.h>
#include <string.h>

static bool released = true, mounted = true, wake, requested_while_held;
static unsigned levels[64], holds[64], unmounts, mounts, requests, led_stops;
static int led_error, unmount_error, wake_error, keep_error_pin = -1, pm_error;
static uint32_t reset_reason, clock_ms, release_at = UINT32_MAX,
                                        last_request_at;
uint32_t bk_misc_get_reset_reason(void) { return reset_reason; }
uint32_t rtos_get_time(void) { return clock_ms; }
int bk_gpio_get_input(gpio_id_t id) {
  assert(id == H2_H202_POWER_KEY_GPIO);
  return released || clock_ms >= release_at;
}
int gpio_dev_unmap(gpio_id_t id) {
  assert(id < 64);
  return 0;
}
int bk_gpio_set_config(gpio_id_t id, const gpio_config_t* config) {
  assert(id < 64);
  if (id == H2_H202_POWER_KEY_GPIO) {
    assert(config->io_mode == GPIO_INPUT_ENABLE);
    assert(config->pull_mode == GPIO_PULL_UP_EN);
  } else {
    assert(config->io_mode == GPIO_OUTPUT_ENABLE);
  }
  return 0;
}
int bk_gpio_set_output_low(gpio_id_t id) {
  levels[id] = 0;
  return 0;
}
int bk_gpio_set_output_high(gpio_id_t id) {
  levels[id] = 1;
  return 0;
}
int bk_gpio_register_wakeup_source(gpio_id_t id, unsigned level) {
  assert(id == H2_H202_POWER_KEY_GPIO && level == GPIO_INT_TYPE_FALLING_EDGE);
  if (wake_error) return wake_error;
  assert(!wake);
  wake = true;
  return 0;
}
int bk_gpio_unregister_wakeup_source(gpio_id_t id) {
  assert(id == H2_H202_POWER_KEY_GPIO && wake);
  wake = false;
  return 0;
}
int bk_gpio_register_lowpower_keep_status(gpio_id_t id,
                                          const gpio_config_t* config) {
  assert(config->io_mode == GPIO_OUTPUT_ENABLE);
  if ((int)id == keep_error_pin) return -1;
  assert(holds[id] == 0);
  holds[id] = 1;
  return 0;
}
int bk_gpio_unregister_lowpower_keep_status(gpio_id_t id) {
  assert(holds[id] == 1);
  holds[id] = 0;
  return 0;
}
int bk_pm_ap_sleep_mode_set(unsigned mode) {
  assert(mode == PM_MODE_DEEP_SLEEP || mode == PM_MODE_DEFAULT);
  if (mode == PM_MODE_DEEP_SLEEP) {
    assert(wake && !mounted);
    requested_while_held =
        requested_while_held || !bk_gpio_get_input(H2_H202_POWER_KEY_GPIO);
    last_request_at = clock_ms;
    ++requests;
    return pm_error;
  }
  return 0;
}
void rtos_delay_milliseconds(unsigned ms) {
  assert((ms > 0 && ms <= 20) || ms == 2000);
  clock_ms += ms;
}
int h2_h202_led_deinit(void) {
  ++led_stops;
  return led_error;
}
int h2_bk7258_board_fs_deinit(void) {
  ++unmounts;
  if (unmount_error) return unmount_error;
  mounted = false;
  return 0;
}
int h2_bk7258_board_fs_init(h2_pal_fs_api_t* api) {
  assert(api && levels[H2_H202_PERIPHERAL_ENABLE_GPIO]);
  ++mounts;
  mounted = true;
  return 0;
}
static void restored(void) {
  assert(mounted && !wake && levels[H2_H202_PERIPHERAL_ENABLE_GPIO]);
  for (unsigned i = 0; i < 64; ++i) assert(holds[i] == 0);
}
static unsigned reboots;
void bk_wdt_force_reboot(void) { ++reboots; }

static void test_wakeup(void) {
  unsigned before = requests;
  clock_ms = 100u;
  released = true;
  assert(h2_bk7258_board_wait_for_wakeup(0u) == H2_PAL_ERR_INVALID_ARG);
  /* Power-on and watchdog reset (including the reset chord) bypass the gate. */
  reset_reason = RESET_SOURCE_POWERON;
  assert(h2_bk7258_board_wait_for_wakeup(2000u) == H2_PAL_OK);
  reset_reason = RESET_SOURCE_WATCHDOG;
  assert(h2_bk7258_board_wait_for_wakeup(2000u) == H2_PAL_OK);
  assert(clock_ms == 100u && requests == before);

  reset_reason = RESET_SOURCE_DEEPPS_GPIO;
  released = false;
  release_at = UINT32_MAX;
  assert(h2_bk7258_board_wait_for_wakeup(2000u) == H2_PAL_OK);
  assert(clock_ms == 2100u && !bk_gpio_get_input(H2_H202_POWER_KEY_GPIO));
  assert(requests == before); /* Still held: proceed without a release. */

  clock_ms = 3000u;
  release_at = 3900u;
  assert(h2_bk7258_board_wait_for_wakeup(2000u) == H2_PAL_ERR_UNAVAILABLE);
  assert(requests == before + 1u && last_request_at == 3900u);
  restored(); /* Short press sleeps immediately, not at the 2 s deadline. */

  clock_ms = 6000u;
  released = true; /* Already released before the wake gate runs. */
  assert(h2_bk7258_board_wait_for_wakeup(2000u) == H2_PAL_ERR_UNAVAILABLE);
  assert(requests == before + 2u && last_request_at == 6000u);
  restored();
  puts("H202 wake hold, short-press sleep and reset bypass passed");
}

int main(void) {
  levels[H2_H202_PERIPHERAL_ENABLE_GPIO] = 1;
  const h2_pal_power_api_t* power = h2_h202_power_api();
  h2_pal_power_capabilities_t capabilities;
  assert(h2_pal_power_get_capabilities(power, &capabilities) == 0);
  assert(capabilities.flags & H2_PAL_POWER_CAPABILITY_SHUTDOWN);
  assert(capabilities.flags & H2_PAL_POWER_CAPABILITY_REBOOT);
  /* A host watchdog stub returns, which must not look like a restart. */
  assert(h2_pal_power_reboot(power, 0) == H2_PAL_ERR_UNAVAILABLE &&
         reboots == 1);
  led_error = H2_PAL_ERR_IO;
  assert(h2_pal_power_shutdown(power, 0) == H2_PAL_ERR_IO && unmounts == 0);
  led_error = 0;
  unmount_error = H2_PAL_ERR_IO;
  assert(h2_pal_power_shutdown(power, 0) == H2_PAL_ERR_IO && requests == 0);
  unmount_error = 0;
  wake_error = -1;
  assert(h2_pal_power_shutdown(power, 0) == H2_PAL_ERR_IO);
  restored();
  wake_error = 0;
  const unsigned rails[] = {53, 7, 13, 37, 4, 54};
  for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); ++i) {
    keep_error_pin = (int)rails[i];
    assert(h2_pal_power_shutdown(power, 0) == H2_PAL_ERR_IO);
    restored();
  }
  keep_error_pin = -1;
  pm_error = -1;
  assert(h2_pal_power_shutdown(power, 0) == H2_PAL_ERR_IO);
  restored();
  pm_error = 0;
  for (unsigned i = 0; i < 2; ++i) {
    released =
        i != 0u; /* The first request reaches PM with Power still held. */
    assert(h2_pal_power_shutdown(power, 0) == H2_PAL_ERR_UNAVAILABLE);
    restored();
  }
  assert(requested_while_held && requests == 3 && mounts == unmounts - 1);
  h2_pal_power_state_t state;
  assert(h2_pal_power_get_state(power, &state) == 0 &&
         state == H2_PAL_POWER_STATE_PREPARING_SHUTDOWN);
  puts("H202 shutdown refusal/retry cleanup passed");
  test_wakeup();
  return 0;
}
