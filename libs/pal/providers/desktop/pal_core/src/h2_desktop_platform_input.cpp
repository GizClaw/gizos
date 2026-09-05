#include "h2_desktop_platform.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

static_assert(
    std::is_array_v<decltype(h2_desktop_nfc_fixture_t::uid)>,
    "the NFC fixture owns inline UID storage; only uid_len is caller-controlled");

std::mutex wifi_settings_mutex;
bool wifi_settings_saved = false;
h2_pal_wifi_sta_config_t wifi_settings_config = {};

int wifi_settings_get(void *, h2_pal_wifi_sta_config_t *out_config) {
  if (out_config == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(wifi_settings_mutex);
  if (!wifi_settings_saved) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_config = wifi_settings_config;
  return H2_PAL_OK;
}

int wifi_settings_set(void *, const h2_pal_wifi_sta_config_t *config) {
  if (config == nullptr || config->ssid_len == 0u ||
      config->ssid_len > H2_PAL_WIFI_SSID_MAX ||
      config->password_len > H2_PAL_WIFI_PASSWORD_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(wifi_settings_mutex);
  wifi_settings_config = *config;
  wifi_settings_config.ssid[config->ssid_len] = '\0';
  wifi_settings_config.password[config->password_len] = '\0';
  wifi_settings_saved = true;
  return H2_PAL_OK;
}

int wifi_settings_clear(void *) {
  std::lock_guard<std::mutex> lock(wifi_settings_mutex);
  wifi_settings_config = {};
  wifi_settings_saved = false;
  return H2_PAL_OK;
}

int wifi_settings_has(void *, int *out_has_config) {
  if (out_has_config == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(wifi_settings_mutex);
  *out_has_config = wifi_settings_saved ? 1 : 0;
  return H2_PAL_OK;
}

const h2_pal_wifi_settings_vtable_t wifi_settings_vtable = {
    wifi_settings_get,
    wifi_settings_set,
    wifi_settings_clear,
    wifi_settings_has,
};
h2_pal_wifi_settings_t wifi_settings = {nullptr, &wifi_settings_vtable};

struct WifiEntry {
  h2_pal_wifi_scan_entry_t entry = {};
  std::array<char, H2_PAL_WIFI_PASSWORD_MAX + 1> password = {};
  size_t password_len = 0u;
};

std::mutex wifi_mutex;
h2_pal_wifi_sta_status_t wifi_status = {};
std::vector<WifiEntry> wifi_entries;
h2_desktop_wifi_scan_outcome_t wifi_outcome =
    H2_DESKTOP_WIFI_SCAN_SUCCESS;
uint32_t wifi_delay_ms = 0u;
constexpr std::array<uint8_t, 6> kWifiStaMac = {
    0x02u, 0x48u, 0x32u, 0x00u, 0x00u, 0x01u};

int wifi_get_status(void *, h2_pal_wifi_sta_status_t *out_status) {
  if (out_status == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(wifi_mutex);
  *out_status = wifi_status;
  return H2_PAL_OK;
}

int wifi_scan(void *, const h2_pal_wifi_scan_request_t *request,
              h2_pal_wifi_scan_result_fn on_result, void *callback_user,
              uint32_t timeout_ms) {
  if (on_result == nullptr ||
      (request != nullptr && request->ssid_len > H2_PAL_WIFI_SSID_MAX)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::vector<WifiEntry> entries;
  h2_desktop_wifi_scan_outcome_t outcome;
  uint32_t delay_ms;
  {
    std::lock_guard<std::mutex> lock(wifi_mutex);
    entries = wifi_entries;
    outcome = wifi_outcome;
    delay_ms = wifi_delay_ms;
  }
  const bool deadline_exceeded =
      delay_ms != 0u && (timeout_ms == 0u || delay_ms > timeout_ms);
  const uint32_t wait_ms = deadline_exceeded ? timeout_ms : delay_ms;
  if (wait_ms != 0u) {
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
  }
  if (deadline_exceeded || outcome == H2_DESKTOP_WIFI_SCAN_TIMEOUT) {
    return H2_PAL_ERR_TIMEOUT;
  }
  if (outcome == H2_DESKTOP_WIFI_SCAN_IO_ERROR) {
    return H2_PAL_ERR_IO;
  }
  for (const WifiEntry &item : entries) {
    if (request != nullptr && request->channel != 0u &&
        request->channel != item.entry.channel) {
      continue;
    }
    if (request != nullptr && request->ssid_len != 0u &&
        (request->ssid_len != item.entry.ssid_len ||
         std::memcmp(request->ssid, item.entry.ssid,
                     request->ssid_len) != 0)) {
      continue;
    }
    if (!on_result(callback_user, &item.entry)) {
      break;
    }
  }
  return H2_PAL_OK;
}

int wifi_connect(void *, const h2_pal_wifi_sta_config_t *config, uint32_t) {
  if (config == nullptr || config->ssid_len == 0u ||
      config->ssid_len > H2_PAL_WIFI_SSID_MAX ||
      config->password_len > H2_PAL_WIFI_PASSWORD_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(wifi_mutex);
  const WifiEntry *matched = nullptr;
  for (const WifiEntry &item : wifi_entries) {
    if (item.entry.ssid_len != config->ssid_len ||
        std::memcmp(item.entry.ssid, config->ssid, config->ssid_len) != 0) {
      continue;
    }
    if (config->bssid_set != 0u &&
        std::memcmp(item.entry.bssid, config->bssid,
                    sizeof(config->bssid)) != 0) {
      continue;
    }
    matched = &item;
    break;
  }

  wifi_status = {};
  std::memcpy(wifi_status.ssid, config->ssid, config->ssid_len);
  wifi_status.ssid_len = config->ssid_len;
  std::memcpy(wifi_status.bssid, config->bssid, sizeof(config->bssid));
  wifi_status.bssid_set = config->bssid_set;
  wifi_status.channel = config->channel;
  wifi_status.rssi = -42;
  if (matched == nullptr) {
    wifi_status.state = H2_PAL_WIFI_STA_STATE_FAILED;
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (config->password_len != matched->password_len ||
      std::memcmp(config->password, matched->password.data(),
                  config->password_len) != 0) {
    wifi_status.state = H2_PAL_WIFI_STA_STATE_FAILED;
    return H2_PAL_ERR_IO;
  }
  wifi_status.channel = matched->entry.channel;
  wifi_status.rssi = matched->entry.rssi;
  std::memcpy(wifi_status.bssid, matched->entry.bssid,
              sizeof(wifi_status.bssid));
  wifi_status.bssid_set = 1u;
  wifi_status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  return H2_PAL_OK;
}

int wifi_disconnect(void *) {
  std::lock_guard<std::mutex> lock(wifi_mutex);
  wifi_status.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
  return H2_PAL_OK;
}

int wifi_get_mac(void *, uint8_t out_mac[6]) {
  if (out_mac == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::copy(kWifiStaMac.begin(), kWifiStaMac.end(), out_mac);
  return H2_PAL_OK;
}

const h2_pal_wifi_sta_vtable_t wifi_sta_vtable = {
    wifi_get_status, wifi_scan,    wifi_connect,
    wifi_disconnect, wifi_get_mac, /*set_power_save=*/nullptr};
h2_pal_wifi_sta_t wifi_sta = {nullptr, &wifi_sta_vtable};

int wifi_ap_start(void *, const h2_pal_wifi_ap_config_t *config, uint32_t) {
  if (config == nullptr || config->ssid_len == 0u ||
      config->ssid_len > H2_PAL_WIFI_SSID_MAX ||
      config->password_len > H2_PAL_WIFI_PASSWORD_MAX ||
      config->max_clients > H2_PAL_WIFI_AP_MAX_CLIENTS) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_ERR_UNSUPPORTED;
}

int wifi_ap_stop(void *, uint32_t) { return H2_PAL_OK; }

int wifi_ap_get_status(void *, h2_pal_wifi_ap_status_t *out_status) {
  if (out_status == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_status = {};
  out_status->state = H2_PAL_WIFI_AP_STATE_STOPPED;
  return H2_PAL_OK;
}

int wifi_ap_get_clients(void *, h2_pal_wifi_ap_client_t *out_clients,
                        size_t max_clients, size_t *out_count) {
  if (out_count == nullptr || (max_clients > 0u && out_clients == nullptr)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_count = 0u;
  return H2_PAL_ERR_UNSUPPORTED;
}

int wifi_ap_get_mac(void *, uint8_t out_mac[6]) {
  return out_mac == nullptr ? H2_PAL_ERR_INVALID_ARG
                            : H2_PAL_ERR_UNSUPPORTED;
}

const h2_pal_wifi_ap_vtable_t wifi_ap_vtable = {
    wifi_ap_start, wifi_ap_stop, wifi_ap_get_status, wifi_ap_get_clients,
    wifi_ap_get_mac};
h2_pal_wifi_ap_t wifi_ap = {nullptr, &wifi_ap_vtable};

struct SimulatedPeripheral {
  h2_desktop_peripheral_config_t config = {};
  h2_pal_button_state_t button_state = H2_PAL_BUTTON_STATE_RELEASED;
  h2_pal_battery_reading_t battery = {};
  h2_pal_periph_pwm_switch_payload_t pwm_payload = {};
  h2_pal_periph_radio_button_payload_t radio_button_payload = {};
  uint16_t duty_x100 = 0u;
};

std::mutex peripheral_mutex;
std::vector<SimulatedPeripheral> peripherals;
h2_pal_periph_id_t nfc_periph_id = 0u;
std::array<uint8_t, H2_PAL_NFC_UID_MAX_LEN> nfc_uid = {};
uint8_t nfc_uid_len = 0u;
std::array<uint8_t, 512> nfc_bytes = {};
size_t nfc_len = 0u;

bool key_valid(h2_desktop_key_t key) {
  return key >= H2_DESKTOP_KEY_SPACE && key <= H2_DESKTOP_KEY_DIGIT_9;
}

const char *key_name(h2_desktop_key_t key, char buffer[2]) {
  if (key >= H2_DESKTOP_KEY_A && key <= H2_DESKTOP_KEY_Z) {
    buffer[0] = static_cast<char>('A' + key - H2_DESKTOP_KEY_A);
    buffer[1] = '\0';
    return buffer;
  }
  if (key == H2_DESKTOP_KEY_DIGIT_0) {
    return "0";
  }
  if (key >= H2_DESKTOP_KEY_DIGIT_1 && key <= H2_DESKTOP_KEY_DIGIT_9) {
    buffer[0] = static_cast<char>('1' + key - H2_DESKTOP_KEY_DIGIT_1);
    buffer[1] = '\0';
    return buffer;
  }
  switch (key) {
  case H2_DESKTOP_KEY_SPACE:
    return "SPACE";
  case H2_DESKTOP_KEY_ENTER:
    return "ENTER";
  case H2_DESKTOP_KEY_ESCAPE:
    return "ESCAPE";
  case H2_DESKTOP_KEY_TAB:
    return "TAB";
  case H2_DESKTOP_KEY_BACKSPACE:
    return "BACKSPACE";
  case H2_DESKTOP_KEY_UP:
    return "UP";
  case H2_DESKTOP_KEY_DOWN:
    return "DOWN";
  case H2_DESKTOP_KEY_LEFT:
    return "LEFT";
  case H2_DESKTOP_KEY_RIGHT:
    return "RIGHT";
  default:
    return "UNKNOWN";
  }
}

const char *key_action(h2_desktop_key_t key) {
  switch (key) {
  case H2_DESKTOP_KEY_LEFT:
    return "Move / navigate left";
  case H2_DESKTOP_KEY_RIGHT:
    return "Move / navigate right";
  case H2_DESKTOP_KEY_UP:
    return "Move / navigate up";
  case H2_DESKTOP_KEY_DOWN:
    return "Move / navigate down";
  case H2_DESKTOP_KEY_SPACE:
    return "Primary action (fire / jump / power)";
  case H2_DESKTOP_KEY_ENTER:
    return "Confirm";
  case H2_DESKTOP_KEY_ESCAPE:
    return "Back";
  case H2_DESKTOP_KEY_TAB:
    return "Record / secondary action";
  case H2_DESKTOP_KEY_BACKSPACE:
    return "Delete / back";
  case H2_DESKTOP_KEY_R:
    return "Reset / retry";
  default:
    return "App control";
  }
}

h2_pal_periph_type_t peripheral_type(h2_desktop_peripheral_kind_t kind) {
  switch (kind) {
  case H2_DESKTOP_PERIPHERAL_BUTTON:
    return H2_PAL_PERIPH_TYPE_SINGLE_BUTTON;
  case H2_DESKTOP_PERIPHERAL_RADIO_BUTTON:
    return H2_PAL_PERIPH_TYPE_RADIO_BUTTON;
  case H2_DESKTOP_PERIPHERAL_NFC_READER:
    return H2_PAL_PERIPH_TYPE_NFC_READER;
  case H2_DESKTOP_PERIPHERAL_IMU:
    return H2_PAL_PERIPH_TYPE_IMU;
  case H2_DESKTOP_PERIPHERAL_BATTERY:
    return H2_PAL_PERIPH_TYPE_BATTERY;
  case H2_DESKTOP_PERIPHERAL_PWM_SWITCH:
    return H2_PAL_PERIPH_TYPE_PWM_SWITCH;
  case H2_DESKTOP_PERIPHERAL_GPIO_IRQ:
    return H2_PAL_PERIPH_TYPE_GPIO_IRQ;
  }
  return H2_PAL_PERIPH_TYPE_ANY;
}

SimulatedPeripheral *find_peripheral(h2_pal_periph_id_t id,
                                     h2_desktop_peripheral_kind_t kind) {
  for (SimulatedPeripheral &peripheral : peripherals) {
    if (peripheral.config.periph_id == id &&
        peripheral.config.kind == kind) {
      return &peripheral;
    }
  }
  return nullptr;
}

void fill_peripheral_info(SimulatedPeripheral &peripheral,
                          h2_pal_periph_info_t *out_info) {
  *out_info = {};
  out_info->id = peripheral.config.periph_id;
  out_info->type = peripheral_type(peripheral.config.kind);
  if (peripheral.config.kind == H2_DESKTOP_PERIPHERAL_PWM_SWITCH) {
    out_info->payload = &peripheral.pwm_payload;
    out_info->payload_size = sizeof(peripheral.pwm_payload);
  } else if (peripheral.config.kind ==
             H2_DESKTOP_PERIPHERAL_RADIO_BUTTON) {
    out_info->payload = &peripheral.radio_button_payload;
    out_info->payload_size = sizeof(peripheral.radio_button_payload);
  }
}

h2_pal_result_t periph_list(void *, h2_pal_periph_type_t type_filter,
                            h2_pal_periph_cb_t callback, void *callback_user) {
  if (callback == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  for (SimulatedPeripheral &peripheral : peripherals) {
    const h2_pal_periph_type_t type =
        peripheral_type(peripheral.config.kind);
    if (type_filter != H2_PAL_PERIPH_TYPE_ANY && type_filter != type) {
      continue;
    }
    h2_pal_periph_info_t info;
    fill_peripheral_info(peripheral, &info);
    const h2_pal_result_t result = callback(callback_user, &info);
    if (result != H2_PAL_OK) {
      return result;
    }
  }
  return H2_PAL_OK;
}

h2_pal_result_t periph_get(void *, h2_pal_periph_id_t id,
                           h2_pal_periph_info_t *out_info) {
  if (out_info == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_info = {};
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  for (SimulatedPeripheral &peripheral : peripherals) {
    if (peripheral.config.periph_id == id) {
      fill_peripheral_info(peripheral, out_info);
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}

const h2_pal_periph_vtable_t periph_vtable = {periph_list, periph_get};
h2_pal_periph_api_t periph_api = {nullptr, &periph_vtable};

h2_pal_result_t read_single_button(void *, h2_pal_periph_id_t id,
                                   h2_pal_single_button_reading_t *out) {
  if (out == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out = {};
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  SimulatedPeripheral *peripheral =
      find_peripheral(id, H2_DESKTOP_PERIPHERAL_BUTTON);
  if (peripheral == nullptr) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  out->id = id;
  out->state = peripheral->button_state;
  return H2_PAL_OK;
}

h2_pal_result_t
read_radio_button_group(void *, h2_pal_periph_id_t group_id,
                        h2_pal_radio_button_group_reading_t *out) {
  if (out == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out = {};
  out->id = group_id;
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  for (const SimulatedPeripheral &peripheral : peripherals) {
    if (peripheral.config.kind == H2_DESKTOP_PERIPHERAL_RADIO_BUTTON &&
        peripheral.radio_button_payload.group_id == group_id &&
        peripheral.button_state == H2_PAL_BUTTON_STATE_PRESSED) {
      out->pressed_button_id = peripheral.config.periph_id;
      break;
    }
  }
  return H2_PAL_OK;
}

const h2_pal_button_vtable_t button_vtable = {read_single_button,
                                               read_radio_button_group};
h2_pal_button_api_t button_api = {nullptr, &button_vtable};

h2_pal_result_t scan_nfc(void *, h2_pal_periph_id_t id,
                         h2_pal_nfc_scan_t *out) {
  if (out == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out = {};
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  SimulatedPeripheral *peripheral =
      find_peripheral(id, H2_DESKTOP_PERIPHERAL_NFC_READER);
  if (peripheral == nullptr) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  out->id = id;
  const bool present =
      id == nfc_periph_id && nfc_len != 0u &&
      peripheral->button_state == H2_PAL_BUTTON_STATE_PRESSED;
  out->stage =
      present ? H2_PAL_NFC_STAGE_DISCOVERED : H2_PAL_NFC_STAGE_ABSENT;
  out->result = H2_PAL_OK;
  if (present) {
    out->tag_type = H2_PAL_NFC_TAG_TYPE_NTAG;
    out->uid_len = nfc_uid_len;
    std::copy_n(nfc_uid.begin(), nfc_uid_len, out->uid);
  }
  return H2_PAL_OK;
}

h2_pal_result_t read_nfc(void *, h2_pal_periph_id_t id,
                         const uint8_t *expected_uid,
                         uint8_t expected_uid_len,
                         h2_pal_nfc_data_type_t requested_type,
                         const h2_pal_mem_api_t *allocator,
                         h2_pal_nfc_data_read_t *out) {
  if (out == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out = {};
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  SimulatedPeripheral *peripheral =
      find_peripheral(id, H2_DESKTOP_PERIPHERAL_NFC_READER);
  if (peripheral == nullptr || id != nfc_periph_id || nfc_len == 0u ||
      peripheral->button_state != H2_PAL_BUTTON_STATE_PRESSED) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  out->id = id;
  if (requested_type != H2_PAL_NFC_DATA_NTAG_PAGES ||
      allocator == nullptr || expected_uid_len != nfc_uid_len ||
      (expected_uid_len != 0u &&
       (expected_uid == nullptr ||
        std::memcmp(expected_uid, nfc_uid.data(), expected_uid_len) != 0))) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  void *bytes = h2_pal_mem_alloc(allocator, nfc_len);
  if (bytes == nullptr) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  std::memcpy(bytes, nfc_bytes.data(), nfc_len);
  out->tag_type = H2_PAL_NFC_TAG_TYPE_NTAG;
  out->uid_len = nfc_uid_len;
  std::copy_n(nfc_uid.begin(), nfc_uid_len, out->uid);
  out->type = H2_PAL_NFC_DATA_NTAG_PAGES;
  out->bytes = static_cast<uint8_t *>(bytes);
  out->len = nfc_len;
  return H2_PAL_OK;
}

const h2_pal_nfc_vtable_t nfc_vtable = {scan_nfc, read_nfc};
h2_pal_nfc_api_t nfc_api = {nullptr, &nfc_vtable};

h2_pal_result_t read_imu(void *, h2_pal_periph_id_t id,
                         h2_pal_imu_reading_t *out) {
  if (out == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out = {};
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  if (find_peripheral(id, H2_DESKTOP_PERIPHERAL_IMU) == nullptr) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  out->id = id;
  out->flags = H2_PAL_IMU_FLAG_NONE;
  return H2_PAL_OK;
}

const h2_pal_imu_vtable_t imu_vtable = {read_imu};
h2_pal_imu_api_t imu_api = {nullptr, &imu_vtable};

h2_pal_result_t read_battery(void *, h2_pal_periph_id_t id,
                             h2_pal_battery_reading_t *out) {
  if (out == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  SimulatedPeripheral *peripheral =
      find_peripheral(id, H2_DESKTOP_PERIPHERAL_BATTERY);
  if (peripheral == nullptr) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out = peripheral->battery;
  return H2_PAL_OK;
}

const h2_pal_input_vtable_t input_vtable = {nullptr, read_battery, nullptr};
h2_pal_input_api_t input_api = {nullptr, &input_vtable};

h2_pal_result_t pwm_set(void *, h2_pal_periph_id_t id, uint16_t duty_x100) {
  if (duty_x100 > H2_PAL_PWM_SWITCH_DUTY_MAX_X100) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  SimulatedPeripheral *peripheral =
      find_peripheral(id, H2_DESKTOP_PERIPHERAL_PWM_SWITCH);
  if (peripheral == nullptr) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  peripheral->duty_x100 = duty_x100;
  return H2_PAL_OK;
}

h2_pal_result_t pwm_get(void *, h2_pal_periph_id_t id,
                        uint16_t *out_duty_x100) {
  if (out_duty_x100 == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  SimulatedPeripheral *peripheral =
      find_peripheral(id, H2_DESKTOP_PERIPHERAL_PWM_SWITCH);
  if (peripheral == nullptr) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_duty_x100 = peripheral->duty_x100;
  return H2_PAL_OK;
}

const h2_pal_pwm_switch_vtable_t pwm_vtable = {pwm_set, pwm_get};
h2_pal_pwm_switch_api_t pwm_api = {nullptr, &pwm_vtable};

struct SimulatedPower {
  std::mutex mutex;
  h2_pal_power_state_t state = H2_PAL_POWER_STATE_RUNNING;
  h2_pal_power_boot_info_t boot_info = {
      H2_PAL_POWER_BOOT_SOURCE_COLD_BOOT,
      0u,
      H2_PAL_POWER_PREVIOUS_TRANSITION_NONE,
      H2_PAL_POWER_RESET_REASON_POWER_ON,
      0u,
      1u,
  };
  bool hold_enabled = false;
  bool deep_sleep_transition_pending = false;
};

SimulatedPower power;

h2_pal_result_t power_capabilities(void *,
                                   h2_pal_power_capabilities_t *out) {
  if (out == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  out->flags = H2_PAL_POWER_CAPABILITY_HOLD |
               H2_PAL_POWER_CAPABILITY_REBOOT |
               H2_PAL_POWER_CAPABILITY_DEEP_SLEEP |
               H2_PAL_POWER_CAPABILITY_BOOT_SOURCE |
               H2_PAL_POWER_CAPABILITY_RESET_REASON;
  return H2_PAL_OK;
}

h2_pal_result_t power_boot_info(void *, h2_pal_power_boot_info_t *out) {
  if (out == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(power.mutex);
  *out = power.boot_info;
  return H2_PAL_OK;
}

h2_pal_result_t power_state(void *, h2_pal_power_state_t *out) {
  if (out == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(power.mutex);
  *out = power.state;
  return H2_PAL_OK;
}

h2_pal_result_t power_set_hold(void *, int enabled) {
  std::lock_guard<std::mutex> lock(power.mutex);
  power.hold_enabled = enabled != 0;
  power.deep_sleep_transition_pending = false;
  if (enabled == 0) {
    power.state = H2_PAL_POWER_STATE_OFF;
    power.boot_info.previous_transition =
        H2_PAL_POWER_PREVIOUS_TRANSITION_SHUTDOWN;
  } else if (power.state == H2_PAL_POWER_STATE_OFF) {
    power.state = H2_PAL_POWER_STATE_RUNNING;
    power.boot_info.source = H2_PAL_POWER_BOOT_SOURCE_COLD_BOOT;
    power.boot_info.source_id = 0u;
    power.boot_info.reset_reason = H2_PAL_POWER_RESET_REASON_POWER_ON;
    power.boot_info.transition_reason = 0u;
    ++power.boot_info.boot_count;
  }
  return H2_PAL_OK;
}

h2_pal_result_t power_get_hold(void *, h2_pal_power_hold_state_t *out) {
  if (out == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(power.mutex);
  out->enabled = power.hold_enabled ? 1 : 0;
  return H2_PAL_OK;
}

h2_pal_result_t power_reboot(void *, uint32_t reason) {
  std::lock_guard<std::mutex> lock(power.mutex);
  power.state = H2_PAL_POWER_STATE_RUNNING;
  power.boot_info.source = H2_PAL_POWER_BOOT_SOURCE_COLD_BOOT;
  power.boot_info.source_id = 0u;
  power.boot_info.previous_transition =
      H2_PAL_POWER_PREVIOUS_TRANSITION_REBOOT;
  power.boot_info.reset_reason = H2_PAL_POWER_RESET_REASON_SOFTWARE;
  power.boot_info.transition_reason = reason;
  ++power.boot_info.boot_count;
  power.deep_sleep_transition_pending = false;
  return H2_PAL_OK;
}

h2_pal_result_t power_deep_sleep(void *, uint32_t reason) {
  std::lock_guard<std::mutex> lock(power.mutex);
  power.state = H2_PAL_POWER_STATE_DEEP_SLEEPING;
  power.boot_info.transition_reason = reason;
  power.deep_sleep_transition_pending = true;
  return H2_PAL_OK;
}

const h2_pal_power_vtable_t power_vtable = {
    power_capabilities,
    power_boot_info,
    power_state,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    power_set_hold,
    power_get_hold,
    nullptr,
    power_reboot,
    nullptr,
    power_deep_sleep,
};
h2_pal_power_api_t power_api = {nullptr, &power_vtable};

int inject_boot(h2_pal_power_boot_source_t source, uint32_t source_id) {
  std::lock_guard<std::mutex> lock(power.mutex);
  if (source == H2_PAL_POWER_BOOT_SOURCE_GPIO_IRQ &&
      power.state != H2_PAL_POWER_STATE_DEEP_SLEEPING) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  power.state = H2_PAL_POWER_STATE_RUNNING;
  power.boot_info.source = source;
  power.boot_info.source_id = source_id;
  power.boot_info.previous_transition =
      source == H2_PAL_POWER_BOOT_SOURCE_GPIO_IRQ
          ? H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP
          : H2_PAL_POWER_PREVIOUS_TRANSITION_NONE;
  power.boot_info.reset_reason =
      source == H2_PAL_POWER_BOOT_SOURCE_GPIO_IRQ
          ? H2_PAL_POWER_RESET_REASON_DEEP_SLEEP
          : H2_PAL_POWER_RESET_REASON_POWER_ON;
  ++power.boot_info.boot_count;
  power.hold_enabled = true;
  power.deep_sleep_transition_pending = false;
  return H2_PAL_OK;
}

const char *power_state_name(h2_pal_power_state_t state) {
  switch (state) {
  case H2_PAL_POWER_STATE_RUNNING:
    return "running";
  case H2_PAL_POWER_STATE_PREPARING_SHUTDOWN:
    return "preparing shutdown";
  case H2_PAL_POWER_STATE_SHUTTING_DOWN:
    return "shutting down";
  case H2_PAL_POWER_STATE_REBOOTING:
    return "rebooting";
  case H2_PAL_POWER_STATE_SLEEPING:
    return "sleeping";
  case H2_PAL_POWER_STATE_DEEP_SLEEPING:
    return "deep sleeping";
  case H2_PAL_POWER_STATE_OFF:
    return "off";
  default:
    return "unknown";
  }
}

const char *peripheral_kind_name(h2_desktop_peripheral_kind_t kind) {
  switch (kind) {
  case H2_DESKTOP_PERIPHERAL_BUTTON:
    return "button";
  case H2_DESKTOP_PERIPHERAL_NFC_READER:
    return "NFC reader";
  case H2_DESKTOP_PERIPHERAL_IMU:
    return "IMU";
  case H2_DESKTOP_PERIPHERAL_BATTERY:
    return "battery";
  case H2_DESKTOP_PERIPHERAL_PWM_SWITCH:
    return "PWM switch";
  case H2_DESKTOP_PERIPHERAL_GPIO_IRQ:
    return "GPIO IRQ";
  case H2_DESKTOP_PERIPHERAL_RADIO_BUTTON:
    return "radio button";
  }
  return "unknown";
}

struct TextWriter {
  char *buffer;
  size_t capacity;
  size_t used;

  void print(const char *format, ...) {
    if (used >= capacity) {
      return;
    }
    va_list args;
    va_start(args, format);
    const int written =
        std::vsnprintf(buffer + used, capacity - used, format, args);
    va_end(args);
    if (written < 0) {
      used = capacity;
      return;
    }
    used = std::min(capacity, used + static_cast<size_t>(written));
  }
};

} // namespace

extern "C" {

h2_pal_wifi_settings_t *h2_desktop_platform_wifi_settings(void) {
  return &wifi_settings;
}

int h2_desktop_platform_configure_wifi_sta(
    const h2_desktop_wifi_sta_config_t *config) {
  if (config == nullptr || config->ssid == nullptr || config->channel == 0u ||
      config->channel > 14u ||
      config->scan_entry_count > H2_PAL_WIFI_SCAN_MAX_RESULTS ||
      (config->scan_entry_count != 0u && config->scan_entries == nullptr) ||
      config->scan_outcome < H2_DESKTOP_WIFI_SCAN_SUCCESS ||
      config->scan_outcome > H2_DESKTOP_WIFI_SCAN_TIMEOUT ||
      config->scan_delay_ms > 60000u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const size_t ssid_len = std::strlen(config->ssid);
  if (ssid_len > H2_PAL_WIFI_SSID_MAX ||
      (config->connected != 0 && ssid_len == 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_wifi_sta_status_t next_status = {};
  std::memcpy(next_status.ssid, config->ssid, ssid_len);
  next_status.ssid_len = ssid_len;
  next_status.channel = config->channel;
  next_status.rssi = config->rssi_dbm;
  next_status.state = config->connected != 0
                          ? H2_PAL_WIFI_STA_STATE_GOT_IP
                          : H2_PAL_WIFI_STA_STATE_IDLE;
  std::vector<WifiEntry> next_entries;
  next_entries.reserve(config->scan_entry_count);
  for (size_t index = 0; index < config->scan_entry_count; ++index) {
    const h2_desktop_wifi_scan_entry_config_t &source =
        config->scan_entries[index];
    if (source.ssid == nullptr || source.ssid_len == 0u ||
        source.ssid_len > H2_PAL_WIFI_SSID_MAX || source.channel == 0u ||
        source.channel > 14u ||
        source.security < H2_PAL_WIFI_SECURITY_OPEN ||
        source.security > H2_PAL_WIFI_SECURITY_ENTERPRISE ||
        source.password_len > H2_PAL_WIFI_PASSWORD_MAX ||
        (source.password_len != 0u && source.password == nullptr) ||
        (source.security == H2_PAL_WIFI_SECURITY_OPEN &&
         source.password_len != 0u) ||
        (source.security != H2_PAL_WIFI_SECURITY_OPEN &&
         source.password_len == 0u)) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    WifiEntry item;
    std::memcpy(item.entry.ssid, source.ssid, source.ssid_len);
    item.entry.ssid_len = source.ssid_len;
    const std::array<uint8_t, 6> bssid = {
        0x02u, 0x48u, 0x32u, 0x10u, 0x00u,
        static_cast<uint8_t>(index + 1u)};
    std::copy(bssid.begin(), bssid.end(), item.entry.bssid);
    item.entry.channel = source.channel;
    item.entry.rssi = source.rssi_dbm;
    item.entry.security = source.security;
    if (source.password_len != 0u) {
      std::memcpy(item.password.data(), source.password, source.password_len);
    }
    item.password_len = source.password_len;
    next_entries.push_back(item);
  }
  std::lock_guard<std::mutex> lock(wifi_mutex);
  wifi_status = next_status;
  wifi_entries = std::move(next_entries);
  wifi_outcome = config->scan_outcome;
  wifi_delay_ms = config->scan_delay_ms;
  return H2_PAL_OK;
}

h2_pal_wifi_sta_t *h2_desktop_platform_wifi_sta(void) { return &wifi_sta; }

h2_pal_wifi_ap_t *h2_desktop_platform_wifi_ap(void) { return &wifi_ap; }

int h2_desktop_platform_configure_peripherals(
    const h2_desktop_peripheral_config_t *configs, size_t count) {
  if ((configs == nullptr && count != 0u) || count > 64u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  for (size_t index = 0; index < count; ++index) {
    const h2_desktop_peripheral_config_t &config = configs[index];
    const bool is_button =
        config.kind == H2_DESKTOP_PERIPHERAL_BUTTON ||
        config.kind == H2_DESKTOP_PERIPHERAL_RADIO_BUTTON;
    if (config.periph_id == 0u ||
        config.kind < H2_DESKTOP_PERIPHERAL_BUTTON ||
        config.kind > H2_DESKTOP_PERIPHERAL_RADIO_BUTTON ||
        (is_button && !key_valid(config.key)) ||
        (config.kind == H2_DESKTOP_PERIPHERAL_RADIO_BUTTON &&
         config.radio_group_id == 0u) ||
        (!is_button && config.simulate == 0) ||
        config.percent_x100 > H2_PAL_PWM_SWITCH_DUTY_MAX_X100 ||
        config.duty_x100 > H2_PAL_PWM_SWITCH_DUTY_MAX_X100) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t previous = 0; previous < index; ++previous) {
      const bool previous_button =
          configs[previous].kind == H2_DESKTOP_PERIPHERAL_BUTTON ||
          configs[previous].kind == H2_DESKTOP_PERIPHERAL_RADIO_BUTTON;
      if (configs[previous].periph_id == config.periph_id ||
          (is_button && previous_button &&
           configs[previous].key == config.key)) {
        return H2_PAL_ERR_INVALID_ARG;
      }
    }
  }

  std::vector<SimulatedPeripheral> next;
  next.reserve(count);
  for (size_t index = 0; index < count; ++index) {
    SimulatedPeripheral peripheral;
    peripheral.config = configs[index];
    peripheral.battery.id = configs[index].periph_id;
    peripheral.battery.flags = configs[index].battery_flags;
    peripheral.battery.voltage_mv = configs[index].voltage_mv;
    peripheral.battery.percent_x100 = configs[index].percent_x100;
    peripheral.radio_button_payload.group_id =
        configs[index].radio_group_id;
    peripheral.pwm_payload.pwm_channel_id = configs[index].periph_id;
    peripheral.pwm_payload.gpio_pin_id = configs[index].periph_id;
    peripheral.pwm_payload.frequency_hz = 5000u;
    peripheral.pwm_payload.duty_resolution_bits = 13u;
    peripheral.duty_x100 = configs[index].duty_x100;
    next.push_back(peripheral);
  }
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  peripherals = std::move(next);
  return H2_PAL_OK;
}

const h2_pal_periph_api_t *h2_desktop_platform_periph_api(void) {
  return &periph_api;
}

const h2_pal_button_api_t *h2_desktop_platform_button_api(void) {
  return &button_api;
}

int h2_desktop_platform_configure_nfc_fixture(
    const h2_desktop_nfc_fixture_t *fixture) {
  if (fixture == nullptr || fixture->periph_id == 0u ||
      fixture->ntag_pages == nullptr || fixture->ntag_pages_len == 0u ||
      fixture->ntag_pages_len > nfc_bytes.size() || fixture->uid_len == 0u ||
      fixture->uid_len > H2_PAL_NFC_UID_MAX_LEN) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  if (find_peripheral(fixture->periph_id,
                      H2_DESKTOP_PERIPHERAL_NFC_READER) == nullptr) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  nfc_periph_id = fixture->periph_id;
  nfc_uid_len = fixture->uid_len;
  std::copy_n(fixture->uid, nfc_uid_len, nfc_uid.begin());
  std::copy_n(fixture->ntag_pages, fixture->ntag_pages_len,
              nfc_bytes.begin());
  nfc_len = fixture->ntag_pages_len;
  return H2_PAL_OK;
}

const h2_pal_nfc_api_t *h2_desktop_platform_nfc_api(void) {
  return &nfc_api;
}

const h2_pal_imu_api_t *h2_desktop_platform_imu_api(void) {
  return &imu_api;
}

const h2_pal_input_api_t *h2_desktop_platform_input_api(void) {
  return &input_api;
}

const h2_pal_pwm_switch_api_t *h2_desktop_platform_pwm_switch_api(void) {
  return &pwm_api;
}

const h2_pal_power_api_t *h2_desktop_platform_power_api(void) {
  return &power_api;
}

int h2_desktop_platform_inject_battery(h2_pal_periph_id_t id,
                                       int32_t voltage_mv,
                                       uint16_t percent_x100,
                                       uint32_t flags) {
  if (percent_x100 > 10000u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  SimulatedPeripheral *peripheral =
      find_peripheral(id, H2_DESKTOP_PERIPHERAL_BATTERY);
  if (peripheral == nullptr) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  peripheral->battery = {id, flags, voltage_mv, 0, percent_x100};
  return H2_PAL_OK;
}

int h2_desktop_platform_inject_cold_boot(void) {
  return inject_boot(H2_PAL_POWER_BOOT_SOURCE_COLD_BOOT, 0u);
}

int h2_desktop_platform_inject_charge_boot(void) {
  {
    std::lock_guard<std::mutex> lock(peripheral_mutex);
    for (SimulatedPeripheral &peripheral : peripherals) {
      if (peripheral.config.kind != H2_DESKTOP_PERIPHERAL_BATTERY) {
        continue;
      }
      peripheral.battery.flags |=
          H2_PAL_BATTERY_PRESENT | H2_PAL_BATTERY_HAS_VOLTAGE_MV |
          H2_PAL_BATTERY_HAS_PERCENT_X100 | H2_PAL_BATTERY_CHARGING;
      peripheral.battery.flags &= ~H2_PAL_BATTERY_FULL;
    }
  }
  return h2_desktop_platform_inject_cold_boot();
}

int h2_desktop_platform_inject_gpio_wake(h2_pal_periph_id_t source_id) {
  if (source_id == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return inject_boot(H2_PAL_POWER_BOOT_SOURCE_GPIO_IRQ, source_id);
}

int h2_desktop_platform_get_power_snapshot(
    h2_desktop_power_snapshot_t *out) {
  if (out == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(power.mutex);
  out->state = power.state;
  out->boot_info = power.boot_info;
  out->hold_enabled = power.hold_enabled ? 1 : 0;
  return H2_PAL_OK;
}

int h2_desktop_platform_handle_key(h2_desktop_key_t key, int pressed,
                                   int repeat) {
  if (!key_valid(key)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (repeat != 0) {
    return H2_PAL_OK;
  }
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  for (SimulatedPeripheral &peripheral : peripherals) {
    if ((peripheral.config.kind == H2_DESKTOP_PERIPHERAL_BUTTON ||
         peripheral.config.kind == H2_DESKTOP_PERIPHERAL_RADIO_BUTTON ||
         peripheral.config.kind == H2_DESKTOP_PERIPHERAL_NFC_READER) &&
        peripheral.config.key == key) {
      peripheral.button_state = pressed != 0 ? H2_PAL_BUTTON_STATE_PRESSED
                                             : H2_PAL_BUTTON_STATE_RELEASED;
    }
  }
  return H2_PAL_OK;
}

void h2_desktop_platform_handle_focus_lost(void) {
  std::lock_guard<std::mutex> lock(peripheral_mutex);
  for (SimulatedPeripheral &peripheral : peripherals) {
    if (peripheral.config.kind == H2_DESKTOP_PERIPHERAL_BUTTON ||
        peripheral.config.kind == H2_DESKTOP_PERIPHERAL_RADIO_BUTTON ||
        peripheral.config.kind == H2_DESKTOP_PERIPHERAL_NFC_READER) {
      peripheral.button_state = H2_PAL_BUTTON_STATE_RELEASED;
    }
  }
}

int h2_desktop_platform_copy_help_text(char *out_help_text,
                                       size_t help_text_size,
                                       size_t *out_help_text_len) {
  if (out_help_text_len == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_help_text_len = 0u;
  if (out_help_text == nullptr || help_text_size == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  TextWriter writer = {out_help_text, help_text_size - 1u, 0u};
  writer.print("Controls / Help\n\nH or ?  Open this help\n");
  {
    std::lock_guard<std::mutex> lock(peripheral_mutex);
    for (const SimulatedPeripheral &peripheral : peripherals) {
      if (peripheral.config.kind != H2_DESKTOP_PERIPHERAL_BUTTON &&
          peripheral.config.kind != H2_DESKTOP_PERIPHERAL_RADIO_BUTTON &&
          peripheral.config.kind != H2_DESKTOP_PERIPHERAL_NFC_READER) {
        continue;
      }
      char key_buffer[2];
      writer.print("%s  %s (peripheral %u)\n",
                   key_name(peripheral.config.key, key_buffer),
                   key_action(peripheral.config.key),
                   peripheral.config.periph_id);
    }
  }
  writer.print("\nSimulator\n\nDisplay: provider-owned RGB565 surface\n");
  {
    std::lock_guard<std::mutex> lock(power.mutex);
    writer.print("Power: %s, hold=%s, boot=%u\n",
                 power_state_name(power.state),
                 power.hold_enabled ? "on" : "off",
                 power.boot_info.boot_count);
  }
  {
    std::lock_guard<std::mutex> lock(wifi_mutex);
    writer.print("Wi-Fi: state=%d, SSID=%s, RSSI=%d, channel=%u\n",
                 wifi_status.state,
                 wifi_status.ssid_len == 0u ? "-" : wifi_status.ssid,
                 wifi_status.rssi, wifi_status.channel);
    writer.print("Wi-Fi scan: %zu result(s), delay=%u ms, outcome=%d\n",
                 wifi_entries.size(), wifi_delay_ms, wifi_outcome);
  }
  {
    std::lock_guard<std::mutex> lock(peripheral_mutex);
    writer.print("Peripherals: %zu\n", peripherals.size());
    for (const SimulatedPeripheral &peripheral : peripherals) {
      writer.print("  #%u %s: ", peripheral.config.periph_id,
                   peripheral_kind_name(peripheral.config.kind));
      if (peripheral.config.kind == H2_DESKTOP_PERIPHERAL_BATTERY) {
        writer.print("%d mV, %u.%02u%%, flags=0x%x\n",
                     peripheral.battery.voltage_mv,
                     peripheral.battery.percent_x100 / 100u,
                     peripheral.battery.percent_x100 % 100u,
                     peripheral.battery.flags);
      } else if (peripheral.config.kind ==
                 H2_DESKTOP_PERIPHERAL_PWM_SWITCH) {
        writer.print("duty=%u.%02u%%\n", peripheral.duty_x100 / 100u,
                     peripheral.duty_x100 % 100u);
      } else {
        writer.print("simulated=%s\n",
                     peripheral.config.simulate != 0 ? "yes" : "no");
      }
    }
  }
  const size_t copied = std::min(writer.used, help_text_size - 1u);
  out_help_text[copied] = '\0';
  *out_help_text_len = copied;
  return H2_PAL_OK;
}

int h2_desktop_platform_take_transition_quit_suppression(void) {
  std::lock_guard<std::mutex> lock(power.mutex);
  const bool pending = power.deep_sleep_transition_pending;
  power.deep_sleep_transition_pending = false;
  return pending ? 1 : 0;
}

} // extern "C"
