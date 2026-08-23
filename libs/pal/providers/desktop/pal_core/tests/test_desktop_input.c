#include "h2_desktop_platform.h"
#include "h2_desktop_test_system_event.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

typedef struct list_state {
  size_t count;
  h2_pal_periph_id_t ids[7];
  h2_pal_periph_type_t types[7];
} list_state_t;

static h2_pal_result_t collect_peripheral(void *user,
                                          const h2_pal_periph_info_t *info) {
  list_state_t *state = (list_state_t *)user;
  assert(state != NULL);
  assert(info != NULL);
  assert(state->count < 7u);
  state->ids[state->count] = info->id;
  state->types[state->count] = info->type;
  state->count += 1u;
  return H2_PAL_OK;
}

static int collect_incoming_call(void *user,
                                 const h2_pal_system_event_t *event) {
  size_t *calls = (size_t *)user;
  assert(event != NULL);
  assert(event->type == H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING);
  assert(event->source_id == 66u);
  assert(event->payload_size == sizeof(h2_pal_modem_call_event_t));
  const h2_pal_modem_call_event_t *call =
      (const h2_pal_modem_call_event_t *)event->payload;
  assert(call->call.direction == H2_PAL_MODEM_CALL_DIRECTION_INCOMING);
  assert(call->call.state == H2_PAL_MODEM_CALL_STATE_INCOMING);
  assert(strcmp(call->call.number, "13800000000") == 0);
  *calls += 1u;
  return H2_PAL_OK;
}

typedef struct call_event_state {
  size_t count;
  h2_pal_modem_call_status_t last;
} call_event_state_t;

static int collect_call_event(void *user,
                              const h2_pal_system_event_t *event) {
  call_event_state_t *state = user;
  assert(state != NULL && event != NULL);
  assert(event->payload_size == sizeof(h2_pal_modem_call_event_t));
  const h2_pal_modem_call_event_t *call = event->payload;
  state->last = call->call;
  state->count += 1u;
  return H2_PAL_OK;
}

typedef struct wifi_scan_state {
  size_t count;
  char ssids[2][H2_PAL_WIFI_SSID_MAX + 1];
} wifi_scan_state_t;

static bool collect_wifi_scan(void *user,
                              const h2_pal_wifi_scan_entry_t *entry) {
  wifi_scan_state_t *state = (wifi_scan_state_t *)user;
  assert(state != NULL && entry != NULL);
  assert(state->count < 2u);
  memcpy(state->ssids[state->count], entry->ssid, entry->ssid_len);
  state->ssids[state->count][entry->ssid_len] = '\0';
  state->count += 1u;
  return true;
}

int main(void) {
  const h2_pal_system_event_api_t *system_events =
      h2_desktop_test_system_event_api();
  const h2_pal_system_event_vtable_t incomplete_vtable = {0};
  const h2_pal_system_event_api_t incomplete_events = {
      .user = NULL,
      .vtable = &incomplete_vtable,
  };
  assert(h2_desktop_platform_modem(NULL) == NULL);
  assert(h2_desktop_platform_modem(&incomplete_events) == NULL);
  assert(h2_desktop_test_system_event_init() == H2_PAL_OK);
  assert(h2_desktop_platform_modem(NULL) == NULL);
  h2_pal_modem_t *modem = h2_desktop_platform_modem(system_events);
  assert(modem != NULL);
  assert(h2_desktop_platform_modem(system_events) == modem);
  assert(h2_desktop_platform_configure_peripherals(NULL, 0u) == H2_PAL_OK);
  list_state_t empty = {0};
  assert(h2_pal_periph_list(h2_desktop_platform_periph_api(),
                            H2_PAL_PERIPH_TYPE_ANY, collect_peripheral,
                            &empty) == H2_PAL_OK);
  assert(empty.count == 0u);

  const h2_desktop_peripheral_config_t peripherals[] = {
      {
          .periph_id = 11u,
          .kind = H2_DESKTOP_PERIPHERAL_BUTTON,
          .key = H2_DESKTOP_KEY_SPACE,
      },
      {
          .periph_id = 22u,
          .kind = H2_DESKTOP_PERIPHERAL_NFC_READER,
          .key = H2_DESKTOP_KEY_N,
          .simulate = 1,
      },
      {
          .periph_id = 33u,
          .kind = H2_DESKTOP_PERIPHERAL_IMU,
          .simulate = 1,
      },
      {
          .periph_id = 44u,
          .kind = H2_DESKTOP_PERIPHERAL_BATTERY,
          .simulate = 1,
          .voltage_mv = 3900,
          .battery_flags = H2_PAL_BATTERY_PRESENT |
                           H2_PAL_BATTERY_HAS_VOLTAGE_MV |
                           H2_PAL_BATTERY_HAS_PERCENT_X100,
          .percent_x100 = 7500u,
      },
      {
          .periph_id = 55u,
          .kind = H2_DESKTOP_PERIPHERAL_PWM_SWITCH,
          .simulate = 1,
          .duty_x100 = 1000u,
      },
      {
          .periph_id = 66u,
          .kind = H2_DESKTOP_PERIPHERAL_GPIO_IRQ,
          .simulate = 1,
      },
      {
          .periph_id = 77u,
          .kind = H2_DESKTOP_PERIPHERAL_RADIO_BUTTON,
          .key = H2_DESKTOP_KEY_DIGIT_7,
          .radio_group_id = UINT32_MAX,
      },
  };
  assert(h2_desktop_platform_configure_peripherals(
             peripherals, sizeof(peripherals) / sizeof(peripherals[0])) ==
         H2_PAL_OK);

  char help_text[8192];
  size_t help_text_len = 0u;
  assert(h2_desktop_platform_copy_help_text(help_text, sizeof(help_text),
                                            &help_text_len) == H2_PAL_OK);
  assert(help_text_len > 0u && help_text_len < sizeof(help_text));
  assert(strstr(help_text, "Controls / Help") != NULL);
  assert(
      strstr(help_text,
             "SPACE  Primary action (fire / jump / power) (peripheral 11)") !=
      NULL);
  assert(strstr(help_text, "#44 battery: 3900 mV, 75.00%") != NULL);
  assert(strstr(help_text, "#55 PWM switch: duty=10.00%") != NULL);
  char tiny_help_text[8];
  assert(h2_desktop_platform_copy_help_text(tiny_help_text,
                                            sizeof(tiny_help_text),
                                            &help_text_len) == H2_PAL_OK);
  assert(help_text_len == sizeof(tiny_help_text) - 1u);
  assert(tiny_help_text[help_text_len] == '\0');
  assert(h2_desktop_platform_copy_help_text(NULL, 0u, &help_text_len) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_desktop_platform_copy_help_text(help_text, sizeof(help_text),
                                            NULL) == H2_PAL_ERR_INVALID_ARG);

  list_state_t listed = {0};
  assert(h2_pal_periph_list(h2_desktop_platform_periph_api(),
                            H2_PAL_PERIPH_TYPE_ANY, collect_peripheral,
                            &listed) == H2_PAL_OK);
  assert(listed.count == 7u);
  assert(listed.ids[0] == 11u &&
         listed.types[0] == H2_PAL_PERIPH_TYPE_SINGLE_BUTTON);
  assert(listed.ids[1] == 22u &&
         listed.types[1] == H2_PAL_PERIPH_TYPE_NFC_READER);
  assert(listed.ids[2] == 33u && listed.types[2] == H2_PAL_PERIPH_TYPE_IMU);
  assert(listed.ids[3] == 44u && listed.types[3] == H2_PAL_PERIPH_TYPE_BATTERY);
  assert(listed.ids[4] == 55u &&
         listed.types[4] == H2_PAL_PERIPH_TYPE_PWM_SWITCH);
  assert(listed.ids[5] == 66u &&
         listed.types[5] == H2_PAL_PERIPH_TYPE_GPIO_IRQ);
  assert(listed.ids[6] == 77u &&
         listed.types[6] == H2_PAL_PERIPH_TYPE_RADIO_BUTTON);

  h2_pal_periph_info_t info;
  assert(h2_pal_periph_get(h2_desktop_platform_periph_api(), 22u, &info) ==
         H2_PAL_OK);
  assert(info.id == 22u && info.type == H2_PAL_PERIPH_TYPE_NFC_READER);
  assert(h2_pal_periph_get(h2_desktop_platform_periph_api(), 999u, &info) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(h2_pal_periph_get(h2_desktop_platform_periph_api(), 77u, &info) ==
         H2_PAL_OK);
  assert(info.payload_size == sizeof(h2_pal_periph_radio_button_payload_t));
  assert(
      ((const h2_pal_periph_radio_button_payload_t *)info.payload)->group_id ==
      UINT32_MAX);

  h2_pal_single_button_reading_t button;
  assert(h2_pal_button_read_single_button(h2_desktop_platform_button_api(), 11u,
                                          &button) == H2_PAL_OK);
  assert(button.id == 11u && button.state == H2_PAL_BUTTON_STATE_RELEASED);
  assert(h2_desktop_platform_handle_key(H2_DESKTOP_KEY_SPACE, 1, 0) ==
         H2_PAL_OK);
  assert(h2_pal_button_read_single_button(h2_desktop_platform_button_api(), 11u,
                                          &button) == H2_PAL_OK);
  assert(button.state == H2_PAL_BUTTON_STATE_PRESSED);
  assert(h2_desktop_platform_handle_key(H2_DESKTOP_KEY_SPACE, 0, 1) ==
         H2_PAL_OK);
  assert(h2_pal_button_read_single_button(h2_desktop_platform_button_api(), 11u,
                                          &button) == H2_PAL_OK);
  assert(button.state == H2_PAL_BUTTON_STATE_PRESSED);
  h2_desktop_platform_handle_focus_lost();
  assert(h2_pal_button_read_single_button(h2_desktop_platform_button_api(), 11u,
                                          &button) == H2_PAL_OK);
  assert(button.state == H2_PAL_BUTTON_STATE_RELEASED);
  assert(h2_pal_button_read_single_button(h2_desktop_platform_button_api(),
                                          999u,
                                          &button) == H2_PAL_ERR_NOT_FOUND);

  h2_pal_radio_button_group_reading_t radio;
  assert(h2_pal_button_read_radio_button_group(h2_desktop_platform_button_api(),
                                               UINT32_MAX,
                                               &radio) == H2_PAL_OK);
  assert(radio.id == UINT32_MAX && radio.pressed_button_id == 0u);
  assert(h2_desktop_platform_handle_key(H2_DESKTOP_KEY_DIGIT_7, 1, 0) ==
         H2_PAL_OK);
  assert(h2_pal_button_read_radio_button_group(h2_desktop_platform_button_api(),
                                               UINT32_MAX,
                                               &radio) == H2_PAL_OK);
  assert(radio.id == UINT32_MAX && radio.pressed_button_id == 77u);
  assert(h2_desktop_platform_handle_key(H2_DESKTOP_KEY_DIGIT_7, 0, 0) ==
         H2_PAL_OK);

  h2_pal_nfc_scan_t scan;
  static const uint8_t ntag_pages[] = {0x03u, 0x01u, 0x00u, 0xfeu};
  h2_desktop_nfc_fixture_t nfc_fixture = {
      .periph_id = 22u,
      .ntag_pages = ntag_pages,
      .ntag_pages_len = sizeof(ntag_pages),
      .uid_len = 4u,
      .uid = {0x04u, 0x25u, 0x10u, 0x42u},
  };
  assert(h2_desktop_platform_configure_nfc_fixture(&nfc_fixture) ==
         H2_PAL_OK);
  assert(h2_pal_nfc_scan_nfc_reader(h2_desktop_platform_nfc_api(), 22u,
                                    &scan) == H2_PAL_OK);
  assert(scan.id == 22u && scan.stage == H2_PAL_NFC_STAGE_ABSENT &&
         scan.result == H2_PAL_OK);
  h2_pal_nfc_data_read_t nfc_data;
  assert(h2_pal_nfc_read_nfc_data(h2_desktop_platform_nfc_api(), 22u, NULL, 0u,
                                  H2_PAL_NFC_DATA_RAW,
                                  h2_desktop_platform_default_allocator(),
                                  &nfc_data) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_desktop_platform_handle_key(H2_DESKTOP_KEY_N, 1, 0) ==
         H2_PAL_OK);
  assert(h2_pal_nfc_scan_nfc_reader(h2_desktop_platform_nfc_api(), 22u,
                                    &scan) == H2_PAL_OK);
  assert(scan.stage == H2_PAL_NFC_STAGE_DISCOVERED &&
         scan.tag_type == H2_PAL_NFC_TAG_TYPE_NTAG && scan.uid_len == 4u);
  assert(h2_pal_nfc_read_nfc_data(
             h2_desktop_platform_nfc_api(), 22u, nfc_fixture.uid,
             nfc_fixture.uid_len, H2_PAL_NFC_DATA_NTAG_PAGES,
             h2_desktop_platform_default_allocator(), &nfc_data) ==
         H2_PAL_OK);
  assert(nfc_data.len == sizeof(ntag_pages) &&
         memcmp(nfc_data.bytes, ntag_pages, sizeof(ntag_pages)) == 0);
  h2_pal_mem_free(h2_desktop_platform_default_allocator(), nfc_data.bytes);
  assert(h2_desktop_platform_handle_key(H2_DESKTOP_KEY_N, 0, 0) ==
         H2_PAL_OK);
  assert(h2_pal_nfc_scan_nfc_reader(h2_desktop_platform_nfc_api(), 999u,
                                    &scan) == H2_PAL_ERR_NOT_FOUND);

  h2_pal_imu_reading_t imu;
  assert(h2_pal_imu_read(h2_desktop_platform_imu_api(), 33u, &imu) ==
         H2_PAL_OK);
  assert(imu.id == 33u && imu.flags == H2_PAL_IMU_FLAG_NONE);
  assert(memcmp(&imu.accel_mg, &(h2_pal_vec3_i32_t){0}, sizeof(imu.accel_mg)) ==
         0);
  assert(memcmp(&imu.gyro_mdps, &(h2_pal_vec3_i32_t){0},
                sizeof(imu.gyro_mdps)) == 0);
  assert(memcmp(&imu.mag_mgauss, &(h2_pal_vec3_i32_t){0},
                sizeof(imu.mag_mgauss)) == 0);
  assert(h2_pal_imu_read(h2_desktop_platform_imu_api(), 999u, &imu) ==
         H2_PAL_ERR_NOT_FOUND);

  h2_pal_battery_reading_t battery;
  assert(h2_pal_input_read_battery(h2_desktop_platform_input_api(), 44u,
                                   &battery) == H2_PAL_OK);
  assert(battery.voltage_mv == 3900 && battery.percent_x100 == 7500u);
  assert(h2_desktop_platform_inject_battery(
             44u, 4100, 10000u, H2_PAL_BATTERY_PRESENT | H2_PAL_BATTERY_FULL) ==
         H2_PAL_OK);
  assert(h2_pal_input_read_battery(h2_desktop_platform_input_api(), 44u,
                                   &battery) == H2_PAL_OK);
  assert(battery.voltage_mv == 4100 && battery.percent_x100 == 10000u);

  const h2_desktop_wifi_sta_config_t wifi_config = {
      .connected = 1,
      .ssid = "Desktop Test",
      .rssi_dbm = -48,
      .channel = 6u,
  };
  assert(h2_desktop_platform_configure_wifi_sta(&wifi_config) == H2_PAL_OK);
  h2_pal_wifi_sta_status_t wifi_status;
  assert(h2_pal_wifi_sta_get_status(h2_desktop_platform_wifi_sta(),
                                    &wifi_status) == H2_PAL_OK);
  assert(wifi_status.state == H2_PAL_WIFI_STA_STATE_GOT_IP);
  assert(wifi_status.ssid_len == strlen("Desktop Test"));
  assert(strcmp(wifi_status.ssid, "Desktop Test") == 0);
  assert(wifi_status.rssi == -48 && wifi_status.channel == 6u);

  char secure_password[] = "secret123";
  const h2_desktop_wifi_scan_entry_config_t scan_entries[] = {
      {.ssid = "Desktop Secure",
       .ssid_len = strlen("Desktop Secure"),
       .password = secure_password,
       .password_len = strlen(secure_password),
       .rssi_dbm = -51,
       .channel = 6u,
       .security = H2_PAL_WIFI_SECURITY_WPA2},
      {.ssid = "Desktop Open",
       .ssid_len = strlen("Desktop Open"),
       .rssi_dbm = -63,
       .channel = 1u,
       .security = H2_PAL_WIFI_SECURITY_OPEN},
  };
  h2_desktop_wifi_sta_config_t simulated_wifi = {
      .connected = 0,
      .ssid = "",
      .rssi_dbm = -90,
      .channel = 1u,
      .scan_outcome = H2_DESKTOP_WIFI_SCAN_SUCCESS,
      .scan_entries = scan_entries,
      .scan_entry_count = 2u,
  };
  assert(h2_desktop_platform_configure_wifi_sta(&simulated_wifi) == H2_PAL_OK);
  secure_password[0] = 'X';

  wifi_scan_state_t scan_state = {0};
  assert(h2_pal_wifi_sta_scan(h2_desktop_platform_wifi_sta(), NULL,
                              collect_wifi_scan, &scan_state,
                              100u) == H2_PAL_OK);
  assert(scan_state.count == 2u);
  assert(strcmp(scan_state.ssids[0], "Desktop Secure") == 0);
  assert(strcmp(scan_state.ssids[1], "Desktop Open") == 0);

  h2_pal_wifi_sta_config_t connect_config = {0};
  memcpy(connect_config.ssid, "Desktop Secure", strlen("Desktop Secure"));
  connect_config.ssid_len = strlen("Desktop Secure");
  memcpy(connect_config.password, "wrong-pass", strlen("wrong-pass"));
  connect_config.password_len = strlen("wrong-pass");
  assert(h2_pal_wifi_sta_connect(h2_desktop_platform_wifi_sta(),
                                 &connect_config, 100u) == H2_PAL_ERR_IO);
  assert(h2_pal_wifi_sta_get_status(h2_desktop_platform_wifi_sta(),
                                    &wifi_status) == H2_PAL_OK);
  assert(wifi_status.state == H2_PAL_WIFI_STA_STATE_FAILED);

  memset(connect_config.password, 0, sizeof(connect_config.password));
  memcpy(connect_config.password, "secret123", strlen("secret123"));
  connect_config.password_len = strlen("secret123");
  assert(h2_pal_wifi_sta_connect(h2_desktop_platform_wifi_sta(),
                                 &connect_config, 100u) == H2_PAL_OK);
  assert(h2_pal_wifi_sta_get_status(h2_desktop_platform_wifi_sta(),
                                    &wifi_status) == H2_PAL_OK);
  assert(wifi_status.state == H2_PAL_WIFI_STA_STATE_GOT_IP);
  assert(wifi_status.rssi == -51 && wifi_status.channel == 6u);

  const h2_desktop_wifi_scan_entry_config_t duplicate_ssid_entries[] = {
      {.ssid = "Duplicate",
       .ssid_len = strlen("Duplicate"),
       .password = "first-pass",
       .password_len = strlen("first-pass"),
       .rssi_dbm = -40,
       .channel = 1u,
       .security = H2_PAL_WIFI_SECURITY_WPA2},
      {.ssid = "Duplicate",
       .ssid_len = strlen("Duplicate"),
       .password = "second-pass",
       .password_len = strlen("second-pass"),
       .rssi_dbm = -60,
       .channel = 11u,
       .security = H2_PAL_WIFI_SECURITY_WPA2},
  };
  const h2_desktop_wifi_sta_config_t duplicate_ssid_wifi = {
      .connected = 0,
      .ssid = "",
      .rssi_dbm = -90,
      .channel = 1u,
      .scan_outcome = H2_DESKTOP_WIFI_SCAN_SUCCESS,
      .scan_entries = duplicate_ssid_entries,
      .scan_entry_count = 2u,
  };
  assert(h2_desktop_platform_configure_wifi_sta(&duplicate_ssid_wifi) ==
         H2_PAL_OK);
  memset(&connect_config, 0, sizeof(connect_config));
  memcpy(connect_config.ssid, "Duplicate", strlen("Duplicate"));
  connect_config.ssid_len = strlen("Duplicate");
  memcpy(connect_config.password, "second-pass", strlen("second-pass"));
  connect_config.password_len = strlen("second-pass");
  connect_config.bssid[0] = 0x02u;
  connect_config.bssid[1] = 0x48u;
  connect_config.bssid[2] = 0x32u;
  connect_config.bssid[3] = 0x10u;
  connect_config.bssid[5] = 0x02u;
  connect_config.bssid_set = 1u;
  assert(h2_pal_wifi_sta_connect(h2_desktop_platform_wifi_sta(),
                                 &connect_config, 100u) == H2_PAL_OK);
  assert(h2_pal_wifi_sta_get_status(h2_desktop_platform_wifi_sta(),
                                    &wifi_status) == H2_PAL_OK);
  assert(wifi_status.channel == 11u);
  assert(memcmp(wifi_status.bssid, connect_config.bssid,
                sizeof(wifi_status.bssid)) == 0);

  simulated_wifi.scan_outcome = H2_DESKTOP_WIFI_SCAN_IO_ERROR;
  assert(h2_desktop_platform_configure_wifi_sta(&simulated_wifi) == H2_PAL_OK);
  scan_state = (wifi_scan_state_t){0};
  assert(h2_pal_wifi_sta_scan(h2_desktop_platform_wifi_sta(), NULL,
                              collect_wifi_scan, &scan_state,
                              100u) == H2_PAL_ERR_IO);
  assert(scan_state.count == 0u);

  simulated_wifi.scan_outcome = H2_DESKTOP_WIFI_SCAN_SUCCESS;
  simulated_wifi.scan_delay_ms = 2u;
  assert(h2_desktop_platform_configure_wifi_sta(&simulated_wifi) == H2_PAL_OK);
  assert(h2_pal_wifi_sta_scan(h2_desktop_platform_wifi_sta(), NULL,
                              collect_wifi_scan, &scan_state,
                              1u) == H2_PAL_ERR_TIMEOUT);
  assert(scan_state.count == 0u);

  simulated_wifi.scan_delay_ms = 0u;
  simulated_wifi.scan_outcome = H2_DESKTOP_WIFI_SCAN_TIMEOUT;
  assert(h2_desktop_platform_configure_wifi_sta(&simulated_wifi) == H2_PAL_OK);
  assert(h2_pal_wifi_sta_scan(h2_desktop_platform_wifi_sta(), NULL,
                              collect_wifi_scan, &scan_state,
                              100u) == H2_PAL_ERR_TIMEOUT);
  assert(scan_state.count == 0u);

  h2_pal_wifi_sta_status_t status_before_invalid;
  assert(h2_pal_wifi_sta_get_status(h2_desktop_platform_wifi_sta(),
                                    &status_before_invalid) == H2_PAL_OK);

  const h2_desktop_wifi_scan_entry_config_t invalid_scan[] = {
      {.ssid = NULL,
       .ssid_len = 1u,
       .rssi_dbm = -70,
       .channel = 1u,
       .security = H2_PAL_WIFI_SECURITY_OPEN},
  };
  const h2_desktop_wifi_sta_config_t invalid_wifi_config = {
      .connected = 0,
      .ssid = "Mutated",
      .rssi_dbm = -90,
      .channel = 1u,
      .scan_entries = invalid_scan,
      .scan_entry_count = 1u,
  };
  assert(h2_desktop_platform_configure_wifi_sta(&invalid_wifi_config) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_wifi_sta_get_status(h2_desktop_platform_wifi_sta(),
                                    &wifi_status) == H2_PAL_OK);
  assert(wifi_status.state == status_before_invalid.state);
  assert(wifi_status.ssid_len == status_before_invalid.ssid_len);
  assert(strcmp(wifi_status.ssid, status_before_invalid.ssid) == 0);
  assert(wifi_status.rssi == status_before_invalid.rssi);
  assert(wifi_status.channel == status_before_invalid.channel);

  char modem_operator[] = "Desktop Operator";
  const h2_desktop_modem_config_t modem_config = {
      .available = 1,
      .mobile_data_enabled = 1,
      .operator_name = modem_operator,
      .rssi_dbm = -82,
      .rat = H2_PAL_MODEM_RAT_LTE,
  };
  assert(h2_desktop_platform_configure_modem(&modem_config) == H2_PAL_OK);
  modem_operator[0] = 'X';
  h2_pal_modem_status_t modem_status;
  assert(h2_pal_modem_get_status(modem, &modem_status) ==
         H2_PAL_OK);
  assert(modem_status.sim == H2_PAL_MODEM_SIM_STATE_READY);
  assert(modem_status.registration == H2_PAL_MODEM_REGISTRATION_HOME);
  assert(modem_status.packet == H2_PAL_MODEM_PACKET_CONNECTED);
  assert(modem_status.rat == H2_PAL_MODEM_RAT_LTE);
  h2_pal_modem_operator_t operator_info;
  assert(h2_pal_modem_get_operator(modem,
                                   &operator_info) == H2_PAL_OK);
  assert(strcmp(operator_info.name, "Desktop Operator") == 0);
  h2_pal_modem_signal_t modem_signal;
  assert(h2_pal_modem_get_signal(modem, &modem_signal) ==
         H2_PAL_OK);
  assert(modem_signal.rssi_dbm == -82 &&
         modem_signal.rat == H2_PAL_MODEM_RAT_LTE);
  assert(h2_pal_modem_data_close(modem, 1000u) ==
         H2_PAL_OK);
  memset(&modem_signal, 0x7f, sizeof(modem_signal));
  assert(h2_pal_modem_get_signal(modem, &modem_signal) ==
         H2_PAL_ERR_UNAVAILABLE);
  assert(modem_signal.rssi_dbm == 0 && modem_signal.ber == 0 &&
         modem_signal.rat == H2_PAL_MODEM_RAT_UNKNOWN);
  assert(h2_pal_modem_data_open(modem, 1000u) ==
         H2_PAL_OK);
  assert(h2_pal_modem_get_signal(modem, &modem_signal) ==
         H2_PAL_OK);

  uint16_t duty_x100 = 0u;
  assert(h2_pal_pwm_switch_get_duty(h2_desktop_platform_pwm_switch_api(), 55u,
                                    &duty_x100) == H2_PAL_OK);
  assert(duty_x100 == 1000u);
  assert(h2_pal_pwm_switch_set_duty(h2_desktop_platform_pwm_switch_api(), 55u,
                                    8000u) == H2_PAL_OK);
  assert(h2_pal_pwm_switch_get_duty(h2_desktop_platform_pwm_switch_api(), 55u,
                                    &duty_x100) == H2_PAL_OK);
  assert(duty_x100 == 8000u);

  assert(h2_pal_power_set_hold(h2_desktop_platform_power_api(), 1) ==
         H2_PAL_OK);
  assert(h2_pal_power_deep_sleep(h2_desktop_platform_power_api(), 7u) ==
         H2_PAL_OK);
  h2_desktop_power_snapshot_t power;
  assert(h2_desktop_platform_get_power_snapshot(&power) == H2_PAL_OK);
  assert(power.state == H2_PAL_POWER_STATE_DEEP_SLEEPING &&
         power.hold_enabled != 0);
  assert(h2_desktop_platform_inject_gpio_wake(66u) == H2_PAL_OK);
  assert(h2_desktop_platform_get_power_snapshot(&power) == H2_PAL_OK);
  assert(power.state == H2_PAL_POWER_STATE_RUNNING);
  assert(power.boot_info.source == H2_PAL_POWER_BOOT_SOURCE_GPIO_IRQ);
  assert(power.boot_info.source_id == 66u);
  assert(h2_desktop_platform_inject_charge_boot() == H2_PAL_OK);
  assert(h2_desktop_platform_get_power_snapshot(&power) == H2_PAL_OK);
  assert(h2_pal_input_read_battery(h2_desktop_platform_input_api(), 44u,
                                   &battery) == H2_PAL_OK);
  assert((battery.flags & H2_PAL_BATTERY_CHARGING) != 0u);
  assert((battery.flags & H2_PAL_BATTERY_FULL) == 0u);
  assert(power.boot_info.source == H2_PAL_POWER_BOOT_SOURCE_COLD_BOOT);
  assert(h2_desktop_platform_inject_cold_boot() == H2_PAL_OK);
  assert(h2_pal_power_set_hold(h2_desktop_platform_power_api(), 0) ==
         H2_PAL_OK);
  assert(h2_desktop_platform_get_power_snapshot(&power) == H2_PAL_OK);
  assert(power.state == H2_PAL_POWER_STATE_OFF && power.hold_enabled == 0);
  assert(h2_desktop_platform_inject_gpio_wake(66u) == H2_PAL_ERR_INVALID_STATE);
  assert(h2_desktop_platform_get_power_snapshot(&power) == H2_PAL_OK);
  assert(power.state == H2_PAL_POWER_STATE_OFF && power.hold_enabled == 0);
  const uint32_t powered_off_boot_count = power.boot_info.boot_count;
  assert(h2_pal_power_set_hold(h2_desktop_platform_power_api(), 1) ==
         H2_PAL_OK);
  assert(h2_desktop_platform_get_power_snapshot(&power) == H2_PAL_OK);
  assert(power.state == H2_PAL_POWER_STATE_RUNNING && power.hold_enabled != 0);
  assert(power.boot_info.source == H2_PAL_POWER_BOOT_SOURCE_COLD_BOOT);
  assert(power.boot_info.source_id == 0u);
  assert(power.boot_info.reset_reason == H2_PAL_POWER_RESET_REASON_POWER_ON);
  assert(power.boot_info.boot_count == powered_off_boot_count + 1u);

  size_t incoming_calls = 0u;
  h2_pal_system_event_subscription_t *subscription = NULL;
  assert(h2_pal_system_event_subscribe(
             system_events, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_INCOMING,
             collect_incoming_call, &incoming_calls,
             &subscription) == H2_PAL_OK);
  assert(h2_desktop_platform_inject_incoming_call(66u) == H2_PAL_OK);
  assert(incoming_calls == 1u);
  h2_pal_system_event_unsubscribe(system_events, subscription);

  call_event_state_t changed_calls = {0};
  assert(h2_pal_system_event_subscribe(
             system_events, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_STATE_CHANGED,
             collect_call_event, &changed_calls, &subscription) == H2_PAL_OK);
  h2_pal_modem_call_request_t dial = {
      .number = "13900000000",
      .timeout_ms = 100u,
  };
  assert(h2_pal_modem_call_dial(modem, &dial) ==
         H2_PAL_OK);
  assert(changed_calls.count == 3u);
  assert(changed_calls.last.direction ==
         H2_PAL_MODEM_CALL_DIRECTION_OUTGOING);
  assert(changed_calls.last.state == H2_PAL_MODEM_CALL_STATE_ACTIVE);
  assert(strcmp(changed_calls.last.number, "13900000000") == 0);
  h2_pal_system_event_unsubscribe(system_events, subscription);

  call_event_state_t ended_calls = {0};
  assert(h2_pal_system_event_subscribe(
             system_events, H2_PAL_SYSTEM_EVENT_TYPE_MODEM_CALL_ENDED,
             collect_call_event, &ended_calls, &subscription) == H2_PAL_OK);
  assert(h2_pal_modem_call_hangup(modem, 100u) ==
         H2_PAL_OK);
  assert(ended_calls.count == 1u);
  assert(ended_calls.last.state == H2_PAL_MODEM_CALL_STATE_ENDED);
  h2_pal_system_event_unsubscribe(system_events, subscription);

  assert(h2_desktop_platform_inject_incoming_call_number(
             66u, "138-0000-0000") == H2_PAL_OK);
  assert(h2_pal_modem_call_answer(modem, 100u) ==
         H2_PAL_OK);
  assert(h2_pal_modem_get_call_status(modem,
                                      &ended_calls.last) == H2_PAL_OK);
  assert(ended_calls.last.state == H2_PAL_MODEM_CALL_STATE_ACTIVE);
  assert(h2_pal_modem_call_hangup(modem, 100u) ==
         H2_PAL_OK);

  h2_desktop_modem_config_t failed_modem = modem_config;
  failed_modem.dial_result = H2_PAL_ERR_TIMEOUT;
  assert(h2_desktop_platform_configure_modem(&failed_modem) == H2_PAL_OK);
  assert(h2_pal_modem_call_dial(modem, &dial) ==
         H2_PAL_ERR_TIMEOUT);
  assert(h2_pal_modem_get_call_status(modem,
                                      &ended_calls.last) == H2_PAL_OK);
  assert(ended_calls.last.state == H2_PAL_MODEM_CALL_STATE_IDLE);
  h2_desktop_test_system_event_deinit();

  h2_desktop_peripheral_config_t invalid = peripherals[1];
  invalid.simulate = 0;
  assert(h2_desktop_platform_configure_peripherals(&invalid, 1u) ==
         H2_PAL_ERR_INVALID_ARG);

  h2_desktop_peripheral_config_t duplicate[] = {peripherals[0], peripherals[0]};
  assert(h2_desktop_platform_configure_peripherals(duplicate, 2u) ==
         H2_PAL_ERR_INVALID_ARG);
  return 0;
}
