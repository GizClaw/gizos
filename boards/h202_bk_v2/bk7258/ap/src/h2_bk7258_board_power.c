#include "h2_bk7258_board_private.h"

#ifdef H2_H202_HOST_TEST
#include "h2_h202_board_test_sdk.h"
#else
#include "components/system.h"
#include "driver/gpio.h"
#include "gpio_driver.h"
#include "modules/pm.h"
#include "os/os.h"
#include "wdt_driver.h"
#endif

#include <stdio.h>

static h2_pal_power_state_t s_state = H2_PAL_POWER_STATE_RUNNING;

static h2_pal_result_t capabilities(void* user,
                                    h2_pal_power_capabilities_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  out->flags = H2_PAL_POWER_CAPABILITY_SHUTDOWN |
               H2_PAL_POWER_CAPABILITY_DEEP_SLEEP |
               H2_PAL_POWER_CAPABILITY_REBOOT;
  return H2_PAL_OK;
}

static h2_pal_result_t state(void* user, h2_pal_power_state_t* out) {
  (void)user;
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = s_state;
  return H2_PAL_OK;
}

/* The launcher has joined every worker before this terminal operation. A
 * failed request leaves a terminal retry state; it never resumes old workers.
 */
static h2_pal_result_t shutdown(void* user, uint32_t reason) {
  (void)user;
  (void)reason;
  s_state = H2_PAL_POWER_STATE_PREPARING_SHUTDOWN;
  int rc = h2_h202_led_deinit();
  if (rc != 0) return rc;
  rc = h2_bk7258_board_fs_deinit();
  if (rc != 0) return rc;
  const unsigned off[] = {
      H2_H202_SLOW_CHARGE_GPIO,      H2_H202_PA_ENABLE_GPIO,
      H2_H202_MODEM_ENABLE_GPIO,     H2_H202_MODEM_VBUS_ENABLE_GPIO,
      H2_H202_WHITE_LED_GPIO,        H2_H202_RGB_GPIO,
      H2_H202_PERIPHERAL_ENABLE_GPIO};
  const gpio_config_t config = {.io_mode = GPIO_OUTPUT_ENABLE,
                                .pull_mode = GPIO_PULL_DOWN_EN,
                                .func_mode = GPIO_SECOND_FUNC_DISABLE};
  bool wake_registered = false;
  unsigned kept = 0;
  /* The shutdown press may still be held. Wake on the next press instead of
   * the current low level; releasing this press must not wake the device. */
  if (bk_gpio_register_wakeup_source((gpio_id_t)H2_H202_POWER_KEY_GPIO,
                                     GPIO_INT_TYPE_FALLING_EDGE) != 0) {
    rc = H2_PAL_ERR_IO;
    goto restore;
  }
  wake_registered = true;
  for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); ++i) {
    gpio_id_t gpio = (gpio_id_t)off[i];
    if (gpio_dev_unmap(gpio) != 0 || bk_gpio_set_config(gpio, &config) != 0 ||
        bk_gpio_set_output_low(gpio) != 0 ||
        bk_gpio_register_lowpower_keep_status(gpio, &config) != 0) {
      rc = H2_PAL_ERR_IO;
      goto restore;
    }
    ++kept;
  }
  /* SDK PM owns the chip-wide CPU/radio shutdown. Keep the persistent
   * management host registered if CP refuses this asynchronous request. */
  s_state = H2_PAL_POWER_STATE_DEEP_SLEEPING;
  rc = bk_pm_ap_sleep_mode_set(PM_MODE_DEEP_SLEEP);
  if (rc == 0) rtos_delay_milliseconds(2000);
  rc = rc == 0 ? H2_PAL_ERR_UNAVAILABLE : H2_PAL_ERR_IO;
restore:
  /* Refusal leaves a saved terminal state, with storage and management still
   * usable for the announced retry. Never silently resume factory workers. */
  if (bk_pm_ap_sleep_mode_set(PM_MODE_DEFAULT) != 0) rc = H2_PAL_ERR_IO;
  if (wake_registered &&
      bk_gpio_unregister_wakeup_source((gpio_id_t)H2_H202_POWER_KEY_GPIO) != 0)
    rc = H2_PAL_ERR_IO;
  for (unsigned i = 0; i < kept; ++i)
    if (bk_gpio_unregister_lowpower_keep_status((gpio_id_t)off[i]) != 0)
      rc = H2_PAL_ERR_IO;
  if (bk_gpio_set_output_high((gpio_id_t)H2_H202_PERIPHERAL_ENABLE_GPIO) != 0)
    rc = H2_PAL_ERR_IO;
  rtos_delay_milliseconds(10);
  h2_pal_fs_api_t fs;
  if (h2_bk7258_board_fs_init(&fs) != 0) rc = H2_PAL_ERR_IO;
  s_state = H2_PAL_POWER_STATE_PREPARING_SHUTDOWN;
  return (h2_pal_result_t)rc;
}

int h2_bk7258_board_wait_for_wakeup(uint32_t hold_ms) {
  if (hold_ms == 0u) return H2_PAL_ERR_INVALID_ARG;
  if (bk_misc_get_reset_reason() != RESET_SOURCE_DEEPPS_GPIO) return H2_PAL_OK;
  const gpio_id_t key = (gpio_id_t)H2_H202_POWER_KEY_GPIO;
  const gpio_config_t input = {.io_mode = GPIO_INPUT_ENABLE,
                               .pull_mode = GPIO_PULL_UP_EN,
                               .func_mode = GPIO_SECOND_FUNC_DISABLE};
  if (gpio_dev_unmap(key) != 0 || bk_gpio_set_config(key, &input) != 0)
    return H2_PAL_ERR_IO;
  printf("H202_POWER_WAKE waiting hold_ms=%lu\r\n", (unsigned long)hold_ms);
  const uint32_t started = rtos_get_time();
  for (;;) {
    if (bk_gpio_get_input(key)) {
      printf("H202_POWER_WAKE short_press sleep\r\n");
      return shutdown(NULL, 0u);
    }
    uint32_t elapsed = rtos_get_time() - started;
    if (elapsed >= hold_ms) break;
    uint32_t left = hold_ms - elapsed;
    rtos_delay_milliseconds(left < 20u ? left : 20u);
  }
  printf("H202_POWER_WAKE accepted\r\n");
  return H2_PAL_OK;
}

/* Chip restart through the SDK watchdog, as the H2Loader BK power API does.
 * The Loader then boots the confirmed App; returning means no restart. */
static h2_pal_result_t power_reboot(void* user, uint32_t reason) {
  (void)user;
  (void)reason;
  bk_wdt_force_reboot();
  return H2_PAL_ERR_UNAVAILABLE;
}

const h2_pal_power_api_t* h2_h202_power_api(void) {
  static const h2_pal_power_vtable_t vtable = {
      .get_capabilities = capabilities,
      .get_state = state,
      .shutdown = shutdown,
      .deep_sleep = shutdown,
      .reboot = power_reboot,
  };
  static const h2_pal_power_api_t api = {.vtable = &vtable};
  return &api;
}
