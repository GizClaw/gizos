#ifndef H2_ZERO_BK_POWER_TEST_SDK_H
#define H2_ZERO_BK_POWER_TEST_SDK_H
#include "h2/pal/hal/h2_pal_power.h"
#include "h2_zero_bk_board_config.h"
#define CONFIG_AUDIO_PLAY 1
#define CONFIG_AUDIO_RECORD 1
#define CONFIG_WIFI_ENABLE 0
#define CONFIG_WIFI_VNET_CONTROLLER 1
#define CONFIG_BLUETOOTH 1

#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
typedef int gpio_id_t;
typedef struct {
  int io_mode, pull_mode, func_mode;
} gpio_config_t;
enum {
  BK_OK = 0,
  GPIO_OUTPUT_ENABLE = 1,
  GPIO_PULL_UP_EN = 2,
  GPIO_PULL_DOWN_EN = 3,
  GPIO_SECOND_FUNC_DISABLE = 4,
  GPIO_INT_TYPE_LOW_LEVEL = 5,
  PM_MODE_DEEP_SLEEP = 6,
  PM_MODE_DEFAULT = 7,
  BK_BLUETOOTH_STATUS_ENABLED = 1
};
enum {
  RESET_SOURCE_POWERON,
  RESET_SOURCE_REBOOT,
  RESET_SOURCE_OTA_REBOOT,
  RESET_SOURCE_WATCHDOG,
  RESET_SOURCE_NMI_WDT,
  RESET_SOURCE_DEEPPS_GPIO,
  RESET_SOURCE_DEEPPS_RTC
};
static int key_pressed, sleeps, config_calls, fail_config, request_calls,
    entered;
static int audio_error, audio_calls, radio_calls, reset_reason, wake_id,
    gpio_cleanup;
static jmp_buf asleep;
static unsigned bk_misc_get_reset_reason(void) { return reset_reason; }
static int bk_gpio_get_wakeup_gpio_id(void) { return wake_id; }
static int reboots;
static void bk_reboot(void) { ++reboots; }
static unsigned critical_depth;
static uint32_t rtos_enter_critical(void) {
  assert(critical_depth == 0u);
  ++critical_depth;
  return 17u;
}
static void rtos_exit_critical(uint32_t level) {
  assert(critical_depth == 1u && level == 17u);
  --critical_depth;
}
static int bk_gpio_get_input(int id) {
  (void)id;
  return !key_pressed;
}
static bool bk_gpio_get_output(int id) {
  (void)id;
  return false;
}
static void rtos_delay_milliseconds(unsigned ms) {
  sleeps += ms;
  if (ms == 2000 && entered) longjmp(asleep, 1);
}
static int bk_gpio_register_lowpower_keep_status(int id,
                                                 const gpio_config_t* cfg) {
  if (id == 13) assert(cfg->pull_mode == GPIO_PULL_UP_EN);
  if (id == 6) assert(cfg->pull_mode == GPIO_PULL_DOWN_EN);
  return ++config_calls == fail_config ? -1 : 0;
}
static int bk_gpio_unregister_lowpower_keep_status(int id) {
  assert(id == H2_ZERO_BK_MODEM_ENABLE_GPIO);
  ++gpio_cleanup;
  return 0;
}
static int bk_gpio_register_wakeup_source(int id, int type) {
  assert(id == 2 && type == GPIO_INT_TYPE_LOW_LEVEL);
  return ++config_calls == fail_config ? -1 : 0;
}
static int bk_gpio_unregister_wakeup_source(int id) {
  assert(id == 2);
  ++gpio_cleanup;
  return 0;
}
static int h2_bk7258_board_audio_prepare_sleep(void) {
  ++audio_calls;
  return audio_error;
}

static int bk_wifi_sta_stop(void) {
  ++radio_calls;
  return 0;
}
static int bk_wifi_ap_stop(void) {
  ++radio_calls;
  return 0;
}
static int bk_bluetooth_deinit(void) {
  ++radio_calls;
  return 0;
}
static int bk_pm_ap_sleep_mode_set(int mode) {
  if (mode == PM_MODE_DEEP_SLEEP) ++request_calls;
  return 0;
}

#define TEST_NATIVE_POWER_FLAGS                                               \
  (H2_PAL_POWER_CAPABILITY_REBOOT | H2_PAL_POWER_CAPABILITY_BOOT_PARTITIONS | \
   H2_PAL_POWER_CAPABILITY_SET_NEXT_BOOT_PARTITION |                          \
   H2_PAL_POWER_CAPABILITY_IMAGE_STATE |                                      \
   H2_PAL_POWER_CAPABILITY_PREPARE_BOOT |                                     \
   H2_PAL_POWER_CAPABILITY_CONFIRM_IMAGE |                                    \
   H2_PAL_POWER_CAPABILITY_WATCHDOG_FEED)

typedef struct native_power_test {
  h2_pal_result_t result;
  uint32_t selected;
  uint32_t prepared;
  h2_pal_power_boot_policy_t policy;
  unsigned confirmations;
  unsigned feeds;
} native_power_test_t;
static native_power_test_t native_power;

static h2_pal_result_t native_power_capabilities(
    void* user, h2_pal_power_capabilities_t* out) {
  assert(user == &native_power && critical_depth == 0u);
  if (native_power.result != H2_PAL_OK) return native_power.result;
  out->flags = TEST_NATIVE_POWER_FLAGS;
  return H2_PAL_OK;
}
static h2_pal_result_t native_power_list(
    void* user, h2_pal_power_boot_partition_cb_t callback,
    void* callback_user) {
  assert(user == &native_power);
  if (native_power.result != H2_PAL_OK) return native_power.result;
  for (uint32_t id = 1u; id <= 2u; ++id) {
    const h2_pal_power_boot_partition_t partition = {.id = id};
    h2_pal_result_t rc = callback(callback_user, &partition);
    if (rc != H2_PAL_OK) return rc;
  }
  return H2_PAL_OK;
}
static h2_pal_result_t native_power_running(
    void* user, h2_pal_power_boot_partition_t* out) {
  assert(user == &native_power);
  if (native_power.result != H2_PAL_OK) return native_power.result;
  *out = (h2_pal_power_boot_partition_t){.id = 1u};
  return H2_PAL_OK;
}
static h2_pal_result_t native_power_next(void* user,
                                         h2_pal_power_boot_partition_t* out) {
  assert(user == &native_power);
  if (native_power.result != H2_PAL_OK) return native_power.result;
  *out = (h2_pal_power_boot_partition_t){.id = native_power.selected};
  return H2_PAL_OK;
}
static h2_pal_result_t native_power_select(void* user, uint32_t id) {
  assert(user == &native_power);
  if (native_power.result != H2_PAL_OK) return native_power.result;
  native_power.selected = id;
  return H2_PAL_OK;
}
static h2_pal_result_t native_power_prepare(
    void* user, const h2_pal_power_boot_request_t* request,
    h2_pal_power_native_evidence_t* out) {
  assert(user == &native_power);
  if (native_power.result != H2_PAL_OK) return native_power.result;
  native_power.prepared = request->partition_id;
  native_power.policy = request->policy;
  *out = (h2_pal_power_native_evidence_t){
      .format = 6u, .size = 1u, .bytes = {0x42u}};
  return H2_PAL_OK;
}
static h2_pal_result_t native_power_image_state(
    void* user, uint32_t id, h2_pal_power_image_state_t* out) {
  assert(user == &native_power && id == 2u);
  if (native_power.result != H2_PAL_OK) return native_power.result;
  *out = (h2_pal_power_image_state_t){
      .state = H2_PAL_POWER_IMAGE_STATE_PENDING_VERIFY,
      .flags = H2_PAL_POWER_IMAGE_FLAG_CONFIRMATION_REQUIRED};
  return H2_PAL_OK;
}
static h2_pal_result_t native_power_confirm(
    void* user, const h2_pal_power_native_evidence_t* evidence,
    h2_pal_power_image_confirmation_t* out) {
  assert(user == &native_power);
  if (native_power.result != H2_PAL_OK) return native_power.result;
  assert(evidence->format == 6u && evidence->size == 1u &&
         evidence->bytes[0] == 0x42u);
  ++native_power.confirmations;
  *out = (h2_pal_power_image_confirmation_t){
      .partition_id = 1u,
      .flags = H2_PAL_POWER_IMAGE_FLAG_RECOVERY_READY,
      .was_pending = 1u};
  return H2_PAL_OK;
}
static h2_pal_result_t native_power_feed(void* user) {
  assert(user == &native_power);
  ++native_power.feeds;
  return native_power.result;
}
static const h2_pal_power_api_t* h2_bk_platform_power_api(void) {
  static const h2_pal_power_vtable_t vtable = {
      .get_capabilities = native_power_capabilities,
      .list_boot_partitions = native_power_list,
      .get_running_boot_partition = native_power_running,
      .get_next_boot_partition = native_power_next,
      .set_next_boot_partition = native_power_select,
      .prepare_next_boot_partition = native_power_prepare,
      .get_image_state = native_power_image_state,
      .confirm_running_image = native_power_confirm,
      .feed_watchdog = native_power_feed,
  };
  static const h2_pal_power_api_t api = {.user = &native_power,
                                         .vtable = &vtable};
  assert(critical_depth == 0u);
  return &api;
}

#endif
