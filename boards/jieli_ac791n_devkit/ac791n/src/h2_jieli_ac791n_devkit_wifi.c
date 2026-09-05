#include "asm/includes.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2_jieli_wl82_platform_core.h"

#ifdef H2_JIELI_NETWORK_ENABLE

#include "lwip.h"
#include "wifi/wifi_connect.h"

#include <string.h>

typedef struct h2_jieli_wifi_state {
  int on;
  enum WIFI_EVENT event;
  h2_pal_wifi_sta_status_t sta;
  h2_pal_wifi_ap_status_t ap;
} h2_jieli_wifi_state_t;

static h2_jieli_wifi_state_t wifi_state;
static void update_sta_snapshot(void);

static void post_system_event(
    h2_pal_system_event_type_t type, const void *payload,
    size_t payload_size) {
  const h2_pal_system_event_t event = {
      .type = type,
      .source_id = 0u,
      .timestamp_ms = timer_get_ms(),
      .payload = payload,
      .payload_size = payload_size,
  };
  /* The runtime owns system-event initialization. Wi-Fi may start earlier
   * during a board probe, in which case there are no subscribers yet. */
  (void)h2_pal_system_event_post(
      h2_jieli_wl82_platform_system_event_api(), &event, 0u);
}

static void post_sta_event(h2_pal_system_event_type_t type) {
  post_system_event(type, &wifi_state.sta, sizeof(wifi_state.sta));
}

static void post_ap_event(h2_pal_system_event_type_t type) {
  h2_pal_wifi_ap_event_t event;
  memset(&event, 0, sizeof(event));
  event.status = wifi_state.ap;
  post_system_event(type, &event, sizeof(event));
}

static uint32_t pack_ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return ((uint32_t)a << 24u) | ((uint32_t)b << 16u) |
         ((uint32_t)c << 8u) | (uint32_t)d;
}

static h2_pal_wifi_security_t map_security(WIFI_802_11_AUTH_MODE mode) {
  switch (mode) {
    case WIFI_AUTH_MODE_OPEN: return H2_PAL_WIFI_SECURITY_OPEN;
    case WIFI_AUTH_MODE_WEP: return H2_PAL_WIFI_SECURITY_WEP;
    case WIFI_AUTH_MODE_WPA: return H2_PAL_WIFI_SECURITY_WPA;
    case WIFI_AUTH_MODE_WPA2PSK: return H2_PAL_WIFI_SECURITY_WPA2;
    case WIFI_AUTH_MODE_WPAWPA2PSK: return H2_PAL_WIFI_SECURITY_WPA_WPA2;
    case WIFI_AUTH_MODE_WPA3SAE:
    case WIFI_AUTH_MODE_WPA3H2E: return H2_PAL_WIFI_SECURITY_WPA3;
    case WIFI_AUTH_MODE_WPA2PSKWPA3SAE:
      return H2_PAL_WIFI_SECURITY_WPA2_WPA3;
    default: return H2_PAL_WIFI_SECURITY_UNKNOWN;
  }
}

static int wifi_event(void *context, enum WIFI_EVENT event) {
  (void)context;
  const h2_pal_wifi_sta_state_t previous_sta_state = wifi_state.sta.state;
  wifi_state.event = event;
  switch (event) {
    case WIFI_EVENT_STA_START:
      wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_IDLE;
      break;
    case WIFI_EVENT_STA_SCAN_COMPLETED:
      wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_IDLE;
      break;
    case WIFI_EVENT_STA_CONNECT_SUCC:
      wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_CONNECTED;
      wifi_state.sta.disconnect_reason = 0;
      post_sta_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTED);
      break;
    case WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC:
      wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
      wifi_state.sta.ip_valid = 1u;
      update_sta_snapshot();
      post_sta_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_GOT_IP);
      break;
    case WIFI_EVENT_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID:
    case WIFI_EVENT_STA_CONNECT_ASSOCIAT_FAIL:
    case WIFI_EVENT_STA_CONNECT_ASSOCIAT_TIMEOUT:
    case WIFI_EVENT_STA_NETWORK_STACK_DHCP_TIMEOUT:
      wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_FAILED;
      wifi_state.sta.disconnect_reason = (int)event;
      wifi_state.sta.ip_valid = 0u;
      if (event == WIFI_EVENT_STA_NETWORK_STACK_DHCP_TIMEOUT) {
        post_sta_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP);
      } else {
        post_sta_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED);
      }
      break;
    case WIFI_EVENT_STA_DISCONNECT:
    case WIFI_EVENT_STA_STOP:
      wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
      wifi_state.sta.ip_valid = 0u;
      if (previous_sta_state == H2_PAL_WIFI_STA_STATE_GOT_IP) {
        post_sta_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP);
      }
      post_sta_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED);
      break;
    case WIFI_EVENT_AP_START:
      wifi_state.ap.state = H2_PAL_WIFI_AP_STATE_STARTED;
      post_ap_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STARTED);
      break;
    case WIFI_EVENT_AP_STOP:
      wifi_state.ap.state = H2_PAL_WIFI_AP_STATE_STOPPED;
      post_ap_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STOPPED);
      break;
    default:
      break;
  }
  return 0;
}

static int ensure_wifi_on(void) {
  if (wifi_state.on || wifi_is_on()) {
    wifi_state.on = 1;
    return H2_PAL_OK;
  }
  wifi_set_event_callback(wifi_event);
  if (wifi_on() != 0) return H2_PAL_ERR_IO;
  wifi_state.on = 1;
  return H2_PAL_OK;
}

static void update_sta_snapshot(void) {
  struct wifi_mode_info mode = {0};
  wifi_get_mode_cur_info(&mode);
  if (mode.mode == STA_MODE && mode.ssid != NULL) {
    size_t length = strlen(mode.ssid);
    if (length > H2_PAL_WIFI_SSID_MAX) length = H2_PAL_WIFI_SSID_MAX;
    memcpy(wifi_state.sta.ssid, mode.ssid, length);
    wifi_state.sta.ssid[length] = '\0';
    wifi_state.sta.ssid_len = length;
  }
  wifi_get_bssid(wifi_state.sta.bssid);
  wifi_state.sta.bssid_set = 1u;
  wifi_state.sta.channel = (uint8_t)wifi_get_channel();
  wifi_state.sta.rssi = wifi_get_rssi();
  if (wifi_state.sta.state == H2_PAL_WIFI_STA_STATE_GOT_IP) {
    struct lan_setting *lan = net_get_lan_info(WIFI_NETIF);
    if (lan != NULL) {
      wifi_state.sta.ip.ip4 = pack_ip4(
          lan->WIRELESS_IP_ADDR0, lan->WIRELESS_IP_ADDR1,
          lan->WIRELESS_IP_ADDR2, lan->WIRELESS_IP_ADDR3);
      wifi_state.sta.ip.netmask4 = pack_ip4(
          lan->WIRELESS_NETMASK0, lan->WIRELESS_NETMASK1,
          lan->WIRELESS_NETMASK2, lan->WIRELESS_NETMASK3);
      wifi_state.sta.ip.gateway4 = pack_ip4(
          lan->WIRELESS_GATEWAY0, lan->WIRELESS_GATEWAY1,
          lan->WIRELESS_GATEWAY2, lan->WIRELESS_GATEWAY3);
    }
  }
}

static int sta_get_status(void *user, h2_pal_wifi_sta_status_t *out_status) {
  (void)user;
  if (out_status == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!wifi_state.on && !wifi_is_on()) {
    memset(out_status, 0, sizeof(*out_status));
    out_status->state = H2_PAL_WIFI_STA_STATE_IDLE;
    return H2_PAL_OK;
  }
  update_sta_snapshot();
  *out_status = wifi_state.sta;
  return H2_PAL_OK;
}

static int sta_scan(
    void *user, const h2_pal_wifi_scan_request_t *request,
    h2_pal_wifi_scan_result_fn on_result, void *callback_user,
    uint32_t timeout_ms) {
  (void)user;
  if (on_result == NULL) return H2_PAL_ERR_INVALID_ARG;
  int result = ensure_wifi_on();
  if (result != H2_PAL_OK) return result;
  wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_SCANNING;
  wifi_state.event = WIFI_EVENT_MODULE_INIT;
  if (wifi_scan_req() != 0) return H2_PAL_ERR_BUSY;
  uint32_t elapsed = 0u;
  while (wifi_state.event != WIFI_EVENT_STA_SCAN_COMPLETED) {
    if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    os_time_dly(1u);
    elapsed += 10u;
  }
  uint32_t count = 0u;
  struct wifi_scan_ssid_info *entries = wifi_get_scan_result(&count);
  for (uint32_t index = 0u; entries != NULL && index < count; ++index) {
    struct wifi_scan_ssid_info *source = &entries[index];
    if (request != NULL && request->ssid_len != 0u &&
        (request->ssid_len != source->ssid_len ||
         memcmp(request->ssid, source->ssid, request->ssid_len) != 0)) {
      continue;
    }
    if (request != NULL && request->channel != 0u &&
        request->channel != source->channel_number) {
      continue;
    }
    h2_pal_wifi_scan_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.ssid_len = source->ssid_len > H2_PAL_WIFI_SSID_MAX
                         ? H2_PAL_WIFI_SSID_MAX
                         : source->ssid_len;
    memcpy(entry.ssid, source->ssid, entry.ssid_len);
    memcpy(entry.bssid, source->mac_addr, sizeof(entry.bssid));
    entry.channel = source->channel_number;
    entry.rssi = source->rssi;
    entry.security = map_security(source->auth_mode);
    if (!on_result(callback_user, &entry)) break;
  }
  wifi_clear_scan_result();
  return H2_PAL_OK;
}

static int sta_connect(
    void *user, const h2_pal_wifi_sta_config_t *config,
    uint32_t timeout_ms) {
  (void)user;
  int result = h2_pal_wifi_settings_validate_sta_config(config);
  if (result != H2_PAL_OK) return result;
  result = ensure_wifi_on();
  if (result != H2_PAL_OK) return result;
  char ssid[H2_PAL_WIFI_SSID_MAX + 1];
  char password[H2_PAL_WIFI_PASSWORD_MAX + 1];
  memcpy(ssid, config->ssid, config->ssid_len);
  ssid[config->ssid_len] = '\0';
  memcpy(password, config->password, config->password_len);
  password[config->password_len] = '\0';
  wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_CONNECTING;
  wifi_state.sta.ip_valid = 0u;
  wifi_state.sta.disconnect_reason = 0;
  wifi_state.sta.ssid_len = config->ssid_len;
  memcpy(wifi_state.sta.ssid, config->ssid, config->ssid_len);
  wifi_state.sta.ssid[config->ssid_len] = '\0';
  post_sta_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTING);
  if (wifi_enter_sta_mode(ssid, password) != 0) return H2_PAL_ERR_IO;
  uint32_t elapsed = 0u;
  while (wifi_state.sta.state != H2_PAL_WIFI_STA_STATE_GOT_IP) {
    if (wifi_state.sta.state == H2_PAL_WIFI_STA_STATE_FAILED) {
      return H2_PAL_ERR_IO;
    }
    if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    os_time_dly(1u);
    elapsed += 10u;
  }
  update_sta_snapshot();
  return H2_PAL_OK;
}

static int wifi_stop(void) {
  if (!wifi_state.on && !wifi_is_on()) return H2_PAL_OK;
  if (wifi_off() != 0) return H2_PAL_ERR_IO;
  wifi_state.on = 0;
  wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
  wifi_state.sta.ip_valid = 0u;
  wifi_state.ap.state = H2_PAL_WIFI_AP_STATE_STOPPED;
  return H2_PAL_OK;
}

static int sta_disconnect(void *user) {
  (void)user;
  return wifi_stop();
}

static int wifi_get_mac_address(void *user, uint8_t out_mac[6]) {
  (void)user;
  if (out_mac == NULL) return H2_PAL_ERR_INVALID_ARG;
  int result = ensure_wifi_on();
  if (result != H2_PAL_OK) return result;
  return wifi_get_mac(out_mac) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

static int ap_start(
    void *user, const h2_pal_wifi_ap_config_t *config,
    uint32_t timeout_ms) {
  (void)user;
  int result = h2_pal_wifi_ap_config_validate(config);
  if (result != H2_PAL_OK) return result;
  result = ensure_wifi_on();
  if (result != H2_PAL_OK) return result;
  char ssid[H2_PAL_WIFI_SSID_MAX + 1];
  char password[H2_PAL_WIFI_PASSWORD_MAX + 1];
  memcpy(ssid, config->ssid, config->ssid_len);
  ssid[config->ssid_len] = '\0';
  memcpy(password, config->password, config->password_len);
  password[config->password_len] = '\0';
  wifi_state.ap.state = H2_PAL_WIFI_AP_STATE_STARTING;
  wifi_state.ap.max_clients = config->max_clients;
  wifi_state.ap.security = config->security;
  wifi_state.ap.hidden = config->hidden;
  wifi_state.ap.ssid_len = config->ssid_len;
  memcpy(wifi_state.ap.ssid, config->ssid, config->ssid_len + 1u);
  if (wifi_enter_ap_mode(ssid, password) != 0) return H2_PAL_ERR_IO;
  uint32_t elapsed = 0u;
  while (wifi_state.ap.state != H2_PAL_WIFI_AP_STATE_STARTED) {
    if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    os_time_dly(1u);
    elapsed += 10u;
  }
  wifi_state.ap.channel = (uint8_t)wifi_get_channel();
  return H2_PAL_OK;
}

static int ap_stop(void *user, uint32_t timeout_ms) {
  (void)user;
  (void)timeout_ms;
  return wifi_stop();
}

static int ap_get_status(void *user, h2_pal_wifi_ap_status_t *out_status) {
  (void)user;
  if (out_status == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_status = wifi_state.ap;
  return H2_PAL_OK;
}

static int ap_get_clients(
    void *user, h2_pal_wifi_ap_client_t *out_clients, size_t max_clients,
    size_t *out_count) {
  (void)user;
  if (out_count == NULL || (out_clients == NULL && max_clients != 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_count = 0u;
  for (int station = 1; station <= H2_PAL_WIFI_AP_MAX_CLIENTS &&
                        *out_count < max_clients; ++station) {
    char *rssi = NULL;
    uint8_t *evm = NULL;
    uint8_t *mac = NULL;
    if (wifi_get_sta_entry_rssi((char)station, &rssi, &evm, &mac) != 0 ||
        mac == NULL) {
      continue;
    }
    h2_pal_wifi_ap_client_t *client = &out_clients[*out_count];
    memset(client, 0, sizeof(*client));
    memcpy(client->mac, mac, sizeof(client->mac));
    client->rssi = rssi == NULL ? 0 : *rssi;
    client->station_id = station;
    ++*out_count;
  }
  wifi_state.ap.client_count = *out_count;
  return H2_PAL_OK;
}

const h2_pal_wifi_sta_api_t *h2_jieli_ac791n_devkit_wifi_sta_api(void) {
  static const h2_pal_wifi_sta_vtable_t vtable = {
      .get_status = sta_get_status,
      .scan = sta_scan,
      .connect = sta_connect,
      .disconnect = sta_disconnect,
      .get_mac = wifi_get_mac_address,
  };
  static const h2_pal_wifi_sta_api_t api = {.user = NULL, .vtable = &vtable};
  return &api;
}

const h2_pal_wifi_ap_api_t *h2_jieli_ac791n_devkit_wifi_ap_api(void) {
  static const h2_pal_wifi_ap_vtable_t vtable = {
      .start = ap_start,
      .stop = ap_stop,
      .get_status = ap_get_status,
      .get_clients = ap_get_clients,
      .get_mac = wifi_get_mac_address,
  };
  static const h2_pal_wifi_ap_api_t api = {.user = NULL, .vtable = &vtable};
  return &api;
}

static int settings_get(
    void *user, h2_pal_wifi_sta_config_t *out_config) {
  (void)user;
  if (out_config == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_pref_namespace_t *name_space = NULL;
  int result = h2_pal_pref_open(
      h2_jieli_ac791n_devkit_pref_api(), "wifi",
      H2_PAL_PREF_OPEN_READ_ONLY, &name_space);
  if (result != H2_PAL_OK) return result;
  void *data = NULL;
  size_t length = 0u;
  const h2_pal_mem_api_t *allocator = h2_jieli_wl82_platform_mem_api();
  result = name_space->get_blob(
      name_space, allocator, "sta-config", &data, &length);
  if (result == H2_PAL_OK && length != sizeof(*out_config)) {
    result = H2_PAL_ERR_FORMAT;
  }
  if (result == H2_PAL_OK) {
    memcpy(out_config, data, sizeof(*out_config));
    result = h2_pal_wifi_settings_validate_sta_config(out_config);
  }
  h2_pal_mem_free(allocator, data);
  int close_result = name_space->close(name_space);
  return result == H2_PAL_OK ? close_result : result;
}

static int settings_set(
    void *user, const h2_pal_wifi_sta_config_t *config) {
  (void)user;
  int result = h2_pal_wifi_settings_validate_sta_config(config);
  if (result != H2_PAL_OK) return result;
  h2_pal_pref_namespace_t *name_space = NULL;
  result = h2_pal_pref_open(
      h2_jieli_ac791n_devkit_pref_api(), "wifi",
      H2_PAL_PREF_OPEN_READ_WRITE, &name_space);
  if (result != H2_PAL_OK) return result;
  result = name_space->set_blob(
      name_space, "sta-config", config, sizeof(*config));
  if (result == H2_PAL_OK) result = name_space->commit(name_space);
  int close_result = name_space->close(name_space);
  return result == H2_PAL_OK ? close_result : result;
}

static int settings_clear(void *user) {
  (void)user;
  h2_pal_pref_namespace_t *name_space = NULL;
  int result = h2_pal_pref_open(
      h2_jieli_ac791n_devkit_pref_api(), "wifi",
      H2_PAL_PREF_OPEN_READ_WRITE, &name_space);
  if (result != H2_PAL_OK) return result;
  result = name_space->remove(name_space, "sta-config");
  if (result == H2_PAL_ERR_NOT_FOUND) result = H2_PAL_OK;
  if (result == H2_PAL_OK) result = name_space->commit(name_space);
  int close_result = name_space->close(name_space);
  return result == H2_PAL_OK ? close_result : result;
}

static int settings_has(void *user, int *out_has_config) {
  (void)user;
  if (out_has_config == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_wifi_sta_config_t config;
  int result = settings_get(NULL, &config);
  if (result == H2_PAL_ERR_NOT_FOUND) {
    *out_has_config = 0;
    return H2_PAL_OK;
  }
  *out_has_config = result == H2_PAL_OK ? 1 : 0;
  return result;
}

const h2_pal_wifi_settings_api_t *
h2_jieli_ac791n_devkit_wifi_settings_api(void) {
  static const h2_pal_wifi_settings_vtable_t vtable = {
      .get_saved_sta_config = settings_get,
      .set_saved_sta_config = settings_set,
      .clear_saved_sta_config = settings_clear,
      .has_saved_sta_config = settings_has,
  };
  static const h2_pal_wifi_settings_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}

#else

const h2_pal_wifi_sta_api_t *h2_jieli_ac791n_devkit_wifi_sta_api(void) {
  return NULL;
}

const h2_pal_wifi_ap_api_t *h2_jieli_ac791n_devkit_wifi_ap_api(void) {
  return NULL;
}

const h2_pal_wifi_settings_api_t *
h2_jieli_ac791n_devkit_wifi_settings_api(void) {
  return NULL;
}

#endif
