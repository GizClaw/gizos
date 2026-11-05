#ifdef H2_ZERO_BK_POWER_HOST_TEST
#include "h2_zero_bk_power_test_sdk.h"
#else
#include "h2_bk7258_board_private.h"
#include "h2_bk_platform_image.h"

#include "components/system.h"
#include "h2_zero_bk_board_config.h"
#include <driver/gpio.h>
#include <modules/pm.h>
#include <modules/wifi.h>
#include <components/bluetooth/bk_dm_bluetooth.h>
#include <os/os.h>
#endif
#include <stdio.h>

#include <string.h>

static h2_pal_power_state_t s_power_state = H2_PAL_POWER_STATE_RUNNING;

static h2_pal_result_t power_get_capabilities(
    void* user, h2_pal_power_capabilities_t* out_capabilities) {
  (void)user;
  if (out_capabilities == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t rc = h2_pal_power_get_capabilities(h2_bk_platform_power_api(),
                                                     out_capabilities);
  if (rc == H2_PAL_OK) {
    out_capabilities->flags |= H2_PAL_POWER_CAPABILITY_REBOOT |
                               H2_PAL_POWER_CAPABILITY_DEEP_SLEEP |
                               H2_PAL_POWER_CAPABILITY_BOOT_SOURCE |
                               H2_PAL_POWER_CAPABILITY_RESET_REASON;
  }
  return rc;
}

static h2_pal_result_t power_get_boot_info(void* user,
                                           h2_pal_power_boot_info_t* out_info) {
  (void)user;
  if (out_info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_info, 0, sizeof(*out_info));
  out_info->source = H2_PAL_POWER_BOOT_SOURCE_COLD_BOOT;
  out_info->previous_transition = H2_PAL_POWER_PREVIOUS_TRANSITION_NONE;
  out_info->reset_reason = H2_PAL_POWER_RESET_REASON_UNKNOWN;
  switch (bk_misc_get_reset_reason()) {
    case RESET_SOURCE_POWERON:
      out_info->reset_reason = H2_PAL_POWER_RESET_REASON_POWER_ON;
      break;
    case RESET_SOURCE_REBOOT:
    case RESET_SOURCE_OTA_REBOOT:
      out_info->source = H2_PAL_POWER_BOOT_SOURCE_UNKNOWN;
      out_info->reset_reason = H2_PAL_POWER_RESET_REASON_SOFTWARE;
      out_info->previous_transition = H2_PAL_POWER_PREVIOUS_TRANSITION_REBOOT;
      break;
    case RESET_SOURCE_WATCHDOG:
    case RESET_SOURCE_NMI_WDT:
      out_info->source = H2_PAL_POWER_BOOT_SOURCE_UNKNOWN;
      out_info->reset_reason = H2_PAL_POWER_RESET_REASON_WATCHDOG;
      break;
    case RESET_SOURCE_DEEPPS_GPIO:
      out_info->source = H2_PAL_POWER_BOOT_SOURCE_GPIO_IRQ;
      out_info->source_id = (uint32_t)bk_gpio_get_wakeup_gpio_id();
      out_info->previous_transition =
          H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP;
      out_info->reset_reason = H2_PAL_POWER_RESET_REASON_DEEP_SLEEP;
      break;
    case RESET_SOURCE_DEEPPS_RTC:
      out_info->source = H2_PAL_POWER_BOOT_SOURCE_TIMER;
      out_info->previous_transition =
          H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP;
      out_info->reset_reason = H2_PAL_POWER_RESET_REASON_DEEP_SLEEP;
      break;
    default:
      out_info->source = H2_PAL_POWER_BOOT_SOURCE_UNKNOWN;
      break;
  }
  /* No persistent boot counter is implemented. */
  out_info->boot_count = 0u;
  return H2_PAL_OK;
}

static h2_pal_result_t power_get_state(void* user,
                                       h2_pal_power_state_t* out_state) {
  (void)user;
  if (out_state == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_state = s_power_state;
  return H2_PAL_OK;
}

static h2_pal_result_t power_reboot(void* user, uint32_t reason) {
  (void)user;
  (void)reason;
  s_power_state = H2_PAL_POWER_STATE_REBOOTING;
  bk_reboot();
  return H2_PAL_OK;
}

/* Terminal transition: callers must first stop App workers and close display.
 * GPIO 13 powers external flash as well as peripherals; retain its rail.
 * RI wake is not advertised until the Zero modem IRQ path is verified. */
static h2_pal_result_t power_deep_sleep(void* user, uint32_t reason) {
  (void)user;
  (void)reason;
  const gpio_id_t key = (gpio_id_t)H2_ZERO_BK_POWER_BUTTON_GPIO;
  for (unsigned waited = 0; !bk_gpio_get_input(key); waited += 10) {
    if (waited >= 5000) {
      printf("H2_ZERO_BK_SLEEP stage=key_release rc=%d\n", H2_PAL_ERR_TIMEOUT);
      return H2_PAL_ERR_TIMEOUT;
    }
    rtos_delay_milliseconds(10);
  }
  int prepare_rc = H2_PAL_OK;
#if CONFIG_AUDIO_PLAY && CONFIG_AUDIO_RECORD
  prepare_rc = h2_bk7258_board_audio_prepare_sleep();
#endif
  if (prepare_rc != H2_PAL_OK) {
    printf("H2_ZERO_BK_SLEEP stage=peripheral_stop rc=%d\n", prepare_rc);
    return prepare_rc;
  }
  const gpio_config_t high = {
      .io_mode = GPIO_OUTPUT_ENABLE,
      .pull_mode = GPIO_PULL_UP_EN,
      .func_mode = GPIO_SECOND_FUNC_DISABLE,
  };
  gpio_config_t modem = high;
  modem.pull_mode = bk_gpio_get_output((gpio_id_t)H2_ZERO_BK_MODEM_ENABLE_GPIO)
                        ? GPIO_PULL_UP_EN
                        : GPIO_PULL_DOWN_EN;
  int rc = bk_gpio_register_lowpower_keep_status(
      (gpio_id_t)H2_ZERO_BK_PERIPHERAL_ENABLE_GPIO, &high);
  if (rc == BK_OK) {
    rc = bk_gpio_register_lowpower_keep_status(
        (gpio_id_t)H2_ZERO_BK_MODEM_ENABLE_GPIO, &modem);
  }
  if (rc == BK_OK) {
    rc = bk_gpio_register_wakeup_source(key, GPIO_INT_TYPE_LOW_LEVEL);
  }
  if (rc != BK_OK) {
    printf("H2_ZERO_BK_SLEEP stage=wake_config rc=%d\n", rc);
    /* GPIO13 belongs to the board lifetime; SDK unregister disables its
     * output and would remove the supply from mounted external Flash. */
    (void)bk_gpio_unregister_lowpower_keep_status(
        (gpio_id_t)H2_ZERO_BK_MODEM_ENABLE_GPIO);
    return H2_PAL_ERR_IO;
  }
  rc = BK_OK;
#if CONFIG_WIFI_ENABLE || CONFIG_WIFI_VNET_CONTROLLER
  rc = bk_wifi_sta_stop();
  if (rc == BK_OK) rc = bk_wifi_ap_stop();
#endif
#if CONFIG_BLUETOOTH || CONFIG_BT || CONFIG_BLE
  if (rc == BK_OK) {
    rc = bk_bluetooth_deinit();
  }
#endif
  if (rc != BK_OK) {
    printf("H2_ZERO_BK_SLEEP stage=radio_stop rc=%d\n", rc);
    goto restore;
  }
  printf("H2_ZERO_BK_SLEEP stage=request wake_gpio=%u\n", (unsigned)key);
  s_power_state = H2_PAL_POWER_STATE_DEEP_SLEEPING;
  rc = bk_pm_ap_sleep_mode_set(PM_MODE_DEEP_SLEEP);
  /* The SDK request is asynchronous and can return OK even when CP refuses.
   * A real deep sleep does not return; report refusal rather than fake sleep.
   */
  if (rc == BK_OK) {
    rtos_delay_milliseconds(2000);
  }
restore:
  (void)bk_pm_ap_sleep_mode_set(PM_MODE_DEFAULT);
  (void)bk_gpio_unregister_wakeup_source(key);
  (void)bk_gpio_unregister_lowpower_keep_status(
      (gpio_id_t)H2_ZERO_BK_MODEM_ENABLE_GPIO);
  s_power_state = H2_PAL_POWER_STATE_RUNNING;
  printf("H2_ZERO_BK_SLEEP stage=not_entered rc=%d\n", rc);
  return rc == BK_OK ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
}

const h2_pal_power_api_t* h2_bk7258_board_power_api(void) {
  static h2_pal_power_vtable_t vtable;
  static h2_pal_power_api_t api;
  const h2_pal_power_api_t* native = h2_bk_platform_power_api();
  const uint32_t level = rtos_enter_critical();
  if (api.vtable == NULL) {
    /* Preserve native bank selection, image evidence and watchdog support. */
    vtable = *native->vtable;
    vtable.get_capabilities = power_get_capabilities;
    vtable.get_boot_info = power_get_boot_info;
    vtable.get_state = power_get_state;
    vtable.reboot = power_reboot;
    vtable.deep_sleep = power_deep_sleep;
    api = *native;
    api.vtable = &vtable;
  }
  rtos_exit_critical(level);
  return &api;
}
