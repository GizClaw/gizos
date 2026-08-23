#ifndef H2_DESKTOP_PLATFORM_H
#define H2_DESKTOP_PLATFORM_H

#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/hal/h2_pal_button.h"
#include "h2/pal/hal/h2_pal_imu.h"
#include "h2/pal/hal/h2_pal_input.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/hal/h2_pal_modem.h"
#include "h2/pal/hal/h2_pal_nfc.h"
#include "h2/pal/hal/h2_pal_periph.h"
#include "h2/pal/hal/h2_pal_power.h"
#include "h2/pal/hal/h2_pal_pwm_switch.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_system_event.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2/pal/hal/h2_pal_wifi_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Keyboard keys accepted by Desktop button bindings. */
typedef enum h2_desktop_key {
  H2_DESKTOP_KEY_SPACE = 1,
  H2_DESKTOP_KEY_ENTER,
  H2_DESKTOP_KEY_ESCAPE,
  H2_DESKTOP_KEY_TAB,
  H2_DESKTOP_KEY_BACKSPACE,
  H2_DESKTOP_KEY_UP,
  H2_DESKTOP_KEY_DOWN,
  H2_DESKTOP_KEY_LEFT,
  H2_DESKTOP_KEY_RIGHT,
  H2_DESKTOP_KEY_A,
  H2_DESKTOP_KEY_B,
  H2_DESKTOP_KEY_C,
  H2_DESKTOP_KEY_D,
  H2_DESKTOP_KEY_E,
  H2_DESKTOP_KEY_F,
  H2_DESKTOP_KEY_G,
  H2_DESKTOP_KEY_H,
  H2_DESKTOP_KEY_I,
  H2_DESKTOP_KEY_J,
  H2_DESKTOP_KEY_K,
  H2_DESKTOP_KEY_L,
  H2_DESKTOP_KEY_M,
  H2_DESKTOP_KEY_N,
  H2_DESKTOP_KEY_O,
  H2_DESKTOP_KEY_P,
  H2_DESKTOP_KEY_Q,
  H2_DESKTOP_KEY_R,
  H2_DESKTOP_KEY_S,
  H2_DESKTOP_KEY_T,
  H2_DESKTOP_KEY_U,
  H2_DESKTOP_KEY_V,
  H2_DESKTOP_KEY_W,
  H2_DESKTOP_KEY_X,
  H2_DESKTOP_KEY_Y,
  H2_DESKTOP_KEY_Z,
  H2_DESKTOP_KEY_DIGIT_0,
  H2_DESKTOP_KEY_DIGIT_1,
  H2_DESKTOP_KEY_DIGIT_2,
  H2_DESKTOP_KEY_DIGIT_3,
  H2_DESKTOP_KEY_DIGIT_4,
  H2_DESKTOP_KEY_DIGIT_5,
  H2_DESKTOP_KEY_DIGIT_6,
  H2_DESKTOP_KEY_DIGIT_7,
  H2_DESKTOP_KEY_DIGIT_8,
  H2_DESKTOP_KEY_DIGIT_9,
} h2_desktop_key_t;

/** Peripheral kinds that Desktop can simulate without external hardware. */
typedef enum h2_desktop_peripheral_kind {
  H2_DESKTOP_PERIPHERAL_BUTTON = 1,
  H2_DESKTOP_PERIPHERAL_NFC_READER,
  H2_DESKTOP_PERIPHERAL_IMU,
  H2_DESKTOP_PERIPHERAL_BATTERY,
  H2_DESKTOP_PERIPHERAL_PWM_SWITCH,
  H2_DESKTOP_PERIPHERAL_GPIO_IRQ,
  H2_DESKTOP_PERIPHERAL_RADIO_BUTTON,
} h2_desktop_peripheral_kind_t;

/** One typed Desktop peripheral configuration. */
typedef struct h2_desktop_peripheral_config {
  h2_pal_periph_id_t periph_id;
  h2_desktop_peripheral_kind_t kind;
  h2_desktop_key_t key;
  int simulate;
  h2_pal_periph_id_t radio_group_id;
  int32_t voltage_mv;
  uint32_t battery_flags;
  uint16_t percent_x100;
  uint16_t duty_x100;
} h2_desktop_peripheral_config_t;

/** Snapshot of the simulated power provider for acceptance tests. */
typedef struct h2_desktop_power_snapshot {
  h2_pal_power_state_t state;
  h2_pal_power_boot_info_t boot_info;
  int hold_enabled;
} h2_desktop_power_snapshot_t;

/** Keyboard-presented deterministic NFC tag fixture. */
typedef struct h2_desktop_nfc_fixture {
  h2_pal_periph_id_t periph_id;
  const uint8_t *ntag_pages;
  size_t ntag_pages_len;
  uint8_t uid_len;
  uint8_t uid[H2_PAL_NFC_UID_MAX_LEN];
} h2_desktop_nfc_fixture_t;

/** One access point returned by the simulated Desktop Wi-Fi scan. */
typedef struct h2_desktop_wifi_scan_entry_config {
  const char *ssid;
  size_t ssid_len;
  const char *password;
  size_t password_len;
  int rssi_dbm;
  uint8_t channel;
  h2_pal_wifi_security_t security;
} h2_desktop_wifi_scan_entry_config_t;

/** Result returned by the simulated Desktop Wi-Fi scan. */
typedef enum h2_desktop_wifi_scan_outcome {
  H2_DESKTOP_WIFI_SCAN_SUCCESS = 0,
  H2_DESKTOP_WIFI_SCAN_IO_ERROR,
  H2_DESKTOP_WIFI_SCAN_TIMEOUT,
} h2_desktop_wifi_scan_outcome_t;

/** Initial state for the simulated Desktop Wi-Fi STA provider. */
typedef struct h2_desktop_wifi_sta_config {
  int connected;
  const char *ssid;
  int rssi_dbm;
  uint8_t channel;
  h2_desktop_wifi_scan_outcome_t scan_outcome;
  uint32_t scan_delay_ms;
  const h2_desktop_wifi_scan_entry_config_t *scan_entries;
  size_t scan_entry_count;
} h2_desktop_wifi_sta_config_t;

/** Initial state for the simulated Desktop modem provider. */
typedef struct h2_desktop_modem_config {
  int available;
  int mobile_data_enabled;
  const char *operator_name;
  int32_t rssi_dbm;
  h2_pal_modem_rat_t rat;
  /** Source attached to deterministic call events; zero uses source 1. */
  h2_pal_periph_id_t call_source_id;
  /** Result returned before an outgoing call emits any state event. */
  h2_pal_result_t dial_result;
} h2_desktop_modem_config_t;

h2_pal_mem_api_t *h2_desktop_platform_default_allocator(void);
h2_pal_mem_api_t *h2_desktop_platform_psram_allocator(void);
h2_pal_mem_api_t *h2_desktop_platform_internal_allocator(void);
h2_pal_mem_api_t *h2_desktop_platform_dma_allocator(void);
const h2_pal_sync_api_t *h2_desktop_platform_sync_api(void);
const h2_pal_queue_api_t *h2_desktop_platform_queue_api(void);
const h2_pal_log_api_t *h2_desktop_platform_log_api(void);
const h2_pal_time_api_t *h2_desktop_platform_time_api(void);
const h2_pal_task_api_t *h2_desktop_platform_task_api(void);
/** Return the configured simulated peripheral enumeration API. */
const h2_pal_periph_api_t *h2_desktop_platform_periph_api(void);
/** Return the keyboard-backed single-button API. */
const h2_pal_button_api_t *h2_desktop_platform_button_api(void);
/** Return the simulated NFC reader API. */
const h2_pal_nfc_api_t *h2_desktop_platform_nfc_api(void);
/** Configure the tag returned while the NFC reader's optional key is held. */
int h2_desktop_platform_configure_nfc_fixture(
    const h2_desktop_nfc_fixture_t *fixture);
/** Return the simulated IMU API. */
const h2_pal_imu_api_t *h2_desktop_platform_imu_api(void);
/** Return the simulated battery input API. */
const h2_pal_input_api_t *h2_desktop_platform_input_api(void);
/** Return the simulated PWM switch API. */
const h2_pal_pwm_switch_api_t *h2_desktop_platform_pwm_switch_api(void);
/** Return the simulated power lifecycle API. */
const h2_pal_power_api_t *h2_desktop_platform_power_api(void);
/**
 * @brief Return the simulated Desktop BLE provider using borrowed events.
 * @param system_event Non-NULL event API kept alive while the provider is used.
 * @return Provider instance, or NULL for invalid or conflicting configuration.
 */
h2_pal_ble_t *h2_desktop_platform_ble(
    const h2_pal_system_event_api_t *system_event);
/** Configure whether the Desktop fake accepts Extended Advertising. */
int h2_desktop_platform_configure_ble_extended_advertising(int supported);
/** Configure whether the Desktop fake accepts Extended Scanning. */
int h2_desktop_platform_configure_ble_extended_scanning(int supported);
/** Inject the result returned by subsequent Desktop fake advertising starts. */
int h2_desktop_platform_configure_ble_advertising_start_result(
    h2_pal_result_t result);
/** Copy the Desktop fake's backend-owned encoded advertising data for tests. */
int h2_desktop_platform_copy_ble_staged_adv_data(uint8_t *out, size_t out_size,
                                                 size_t *out_len);
/** Copy one multiple-set fake payload for deterministic tests. */
int h2_desktop_platform_copy_ble_adv_set_data(h2_pal_ble_adv_set_t *set,
                                              uint8_t *out, size_t out_size,
                                              size_t *out_len);
h2_pal_wifi_sta_t *h2_desktop_platform_wifi_sta(void);
h2_pal_wifi_ap_t *h2_desktop_platform_wifi_ap(void);
h2_pal_wifi_settings_t *h2_desktop_platform_wifi_settings(void);
/**
 * @brief Return the simulated Desktop modem provider using borrowed events.
 * @param system_event Non-NULL event API kept alive while the provider is used.
 * @return Provider instance, or NULL for invalid or conflicting configuration.
 */
h2_pal_modem_t *h2_desktop_platform_modem(
    const h2_pal_system_event_api_t *system_event);
/**
 * @brief Replace the simulated Wi-Fi STA and scan state.
 *
 * The function copies all strings and scan entries. Configuration must remain
 * unchanged while another thread is using the Wi-Fi provider.
 *
 * @param config Valid connected state and zero or more scan entries.
 * @return H2_PAL_OK or H2_PAL_ERR_INVALID_ARG.
 */
int h2_desktop_platform_configure_wifi_sta(
    const h2_desktop_wifi_sta_config_t *config);
/**
 * @brief Replace the simulated modem state.
 *
 * The function copies operator_name. Configuration must remain unchanged while
 * another thread is using the modem provider.
 *
 * @param config Valid modem availability, connection, operator, signal and RAT.
 * @return H2_PAL_OK or H2_PAL_ERR_INVALID_ARG.
 */
int h2_desktop_platform_configure_modem(
    const h2_desktop_modem_config_t *config);
/**
 * @brief Replace the complete simulated peripheral set before runtime
 * initialization.
 *
 * The function copies the array. Configuration must remain unchanged while a
 * Runtime or another consumer is enumerating or reading peripherals.
 *
 * @param peripherals Peripheral array, or NULL when peripheral_count is zero.
 * @param peripheral_count Number of configured peripherals.
 * @return H2_PAL_OK or H2_PAL_ERR_INVALID_ARG.
 */
int h2_desktop_platform_configure_peripherals(
    const h2_desktop_peripheral_config_t *peripherals, size_t peripheral_count);
/**
 * @brief Apply one keyboard transition to every matching button peripheral.
 * @param key Configured Desktop key.
 * @param pressed Nonzero for pressed and zero for released.
 * @param repeat Nonzero for an SDL repeat event, which is ignored.
 * @return H2_PAL_OK or H2_PAL_ERR_INVALID_ARG.
 */
int h2_desktop_platform_handle_key(h2_desktop_key_t key, int pressed,
                                   int repeat);
int h2_desktop_platform_inject_battery(h2_pal_periph_id_t id,
                                       int32_t voltage_mv,
                                       uint16_t percent_x100, uint32_t flags);
int h2_desktop_platform_inject_cold_boot(void);
/**
 * @brief Simulate the power-on caused by connecting a charge cable.
 *
 * Every configured simulated battery is marked CHARGING (and not FULL) before
 * the hardware COLD_BOOT is injected. The portable App combines those two
 * signals to classify its business-level charge-cable wake reason.
 */
int h2_desktop_platform_inject_charge_boot(void);
int h2_desktop_platform_inject_gpio_wake(h2_pal_periph_id_t source_id);
/** Inject a deterministic incoming call from the default whitelist fixture. */
int h2_desktop_platform_inject_incoming_call(h2_pal_periph_id_t source_id);
/** Inject an incoming call with an explicit complete phone number. */
int h2_desktop_platform_inject_incoming_call_number(
    h2_pal_periph_id_t source_id, const char *number);
/** Inject one exact Modem call event for ordering and failure tests. */
int h2_desktop_platform_inject_modem_call_event(
    h2_pal_periph_id_t source_id, h2_pal_system_event_type_t type,
    const h2_pal_modem_call_status_t *status);
int h2_desktop_platform_get_power_snapshot(
    h2_desktop_power_snapshot_t *out_snapshot);
/** Release all simulated buttons when the SDL window loses focus. */
void h2_desktop_platform_handle_focus_lost(void);
/**
 * @brief Copy the current Desktop controls and simulator state as UTF-8 text.
 *
 * The snapshot is generated when called. The output is always NUL-terminated
 * when help_text_size is nonzero; content may be truncated to fit.
 *
 * @param out_help_text Caller-provided output buffer.
 * @param help_text_size Output buffer capacity including the NUL terminator.
 * @param out_help_text_len Receives the copied byte length excluding NUL.
 * @return H2_PAL_OK or H2_PAL_ERR_INVALID_ARG.
 */
int h2_desktop_platform_copy_help_text(char *out_help_text,
                                       size_t help_text_size,
                                       size_t *out_help_text_len);
#ifdef __cplusplus
}
#endif

#endif
