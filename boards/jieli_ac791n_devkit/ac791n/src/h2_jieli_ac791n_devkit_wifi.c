#include "asm/includes.h"

#include "h2_wifi_sta.h"

#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_network.h"
#include "h2/pal/hal/h2_pal_wifi.h"
#include "h2_jieli_wl82_platform_core.h"
#include "h2_jieli_wl82_atomic.h"

#ifdef H2_JIELI_NETWORK_ENABLE

#include "lwip.h"
#include "wifi/wifi_connect.h"

#include <string.h>

/* Pinned SDK eb04f196: wifi_get_sta_entry_rssi rejects WCID >= 5 and
 * _MAC_TABLE.Content has five entries (wl_wifi{,_ap,_sfc}.a IR).
 * wifi_app_timer_func's i < 8 is only a scan ceiling: it breaks on error.
 * No station-slot constant is exported by the SDK's public Wi-Fi headers. */
enum { H2_JIELI_AP_STATION_SLOTS = 5 };
#ifdef MAX_LEN_OF_MAC_TABLE
_Static_assert(H2_JIELI_AP_STATION_SLOTS == MAX_LEN_OF_MAC_TABLE,
               "Recheck the SDK AP station-slot capacity");
#endif

typedef struct h2_jieli_wifi_state {
  int on;
  h2_pal_wifi_sta_status_t sta;
  h2_pal_wifi_ap_status_t ap;
  h2_pal_wifi_ap_client_t ap_clients[H2_JIELI_AP_STATION_SLOTS];
} h2_jieli_wifi_state_t;

static h2_jieli_wifi_state_t wifi_state;
static uint32_t wifi_state_gate;
static uint32_t wifi_sta_generation;
static unsigned wifi_callbacks_active;

static void wifi_state_lock(void) {
  for (;;) {
    uint32_t expected = 0u;
    if (h2_jieli_atomic_cas_u32(&wifi_state_gate, &expected, 1u)) return;
    os_time_dly(1u);
  }
}

static void wifi_state_unlock(void) {
  h2_jieli_atomic_store_u32(&wifi_state_gate, 0u);
}

static void update_sta_snapshot(h2_pal_wifi_sta_status_t *status);

enum { SCAN_IDLE, SCAN_PENDING, SCAN_READY, SCAN_ABANDONED, SCAN_CLEANING, SCAN_REAPABLE };
static unsigned scan_phase;

static void scan_completed(void) {
  unsigned expected = SCAN_PENDING;
  if (__atomic_compare_exchange_n(&scan_phase, &expected, SCAN_READY, 0,
                                   __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return;
  expected = SCAN_ABANDONED;
  /* SDK callbacks run under network_hsm_mtx. Clearing results synchronously
   * dispatches through that same mutex, so only publish task-side work here. */
  (void)__atomic_compare_exchange_n(&scan_phase, &expected, SCAN_REAPABLE, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

static void scan_reap_completed(void) {
  unsigned expected = SCAN_REAPABLE;
  if (__atomic_compare_exchange_n(&scan_phase, &expected, SCAN_CLEANING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
    wifi_clear_scan_result();
    __atomic_store_n(&scan_phase, SCAN_IDLE, __ATOMIC_RELEASE);
  }
}

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

static void post_sta_event(
    h2_pal_system_event_type_t type, const h2_pal_wifi_sta_status_t *status) {
  post_system_event(type, status, sizeof(*status));
}

static void post_ap_event(
    h2_pal_system_event_type_t type, const h2_pal_wifi_ap_status_t *status) {
  h2_pal_wifi_ap_event_t event;
  memset(&event, 0, sizeof(event));
  event.status = *status;
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
  if (event == WIFI_EVENT_STA_SCAN_COMPLETED) {
    scan_completed();
    return 0;
  }
  /* These SDK queries execute in its serialized Wi-Fi event context, never
   * under the PAL gate. A newer PAL transition invalidates this refresh. */
  h2_pal_wifi_sta_status_t native_status;
  memset(&native_status, 0, sizeof(native_status));
  const int refresh = event == WIFI_EVENT_STA_CONNECT_SUCC ||
      event == WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC;
  wifi_state_lock();
  ++wifi_callbacks_active;
  const uint32_t generation = wifi_sta_generation;
  wifi_state_unlock();
  if (refresh) {
    native_status.state = event == WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC
        ? H2_PAL_WIFI_STA_STATE_GOT_IP : H2_PAL_WIFI_STA_STATE_CONNECTED;
    update_sta_snapshot(&native_status);
  }
  if (event == WIFI_EVENT_AP_START) wifi_rxfilter_cfg(1);

  h2_pal_system_event_type_t types[2];
  size_t count = 0u;
  int ap_event = 0;
  h2_pal_system_event_type_t ap_type = 0;
  h2_pal_wifi_sta_status_t sta_status;
  h2_pal_wifi_ap_status_t ap_status;
  h2_pal_wifi_ap_client_event_t client_event;
  h2_pal_system_event_type_t client_type = 0;
  wifi_state_lock();
  if (refresh && generation != wifi_sta_generation) {
    --wifi_callbacks_active;
    wifi_state_unlock();
    return 0;
  }
  const h2_pal_wifi_sta_state_t previous_sta_state = wifi_state.sta.state;
  int sta_changed = 1;
  switch (event) {
    case WIFI_EVENT_STA_START:
      wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_IDLE;
      break;
    case WIFI_EVENT_STA_CONNECT_SUCC:
    case WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC:
      wifi_state.sta.state = native_status.state;
      wifi_state.sta.disconnect_reason = 0;
      memcpy(wifi_state.sta.bssid, native_status.bssid, sizeof(native_status.bssid));
      wifi_state.sta.bssid_set = native_status.bssid_set;
      wifi_state.sta.channel = native_status.channel;
      wifi_state.sta.rssi = native_status.rssi;
      if (event == WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC && !native_status.ip_valid) {
        wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_FAILED;
        wifi_state.sta.disconnect_reason = H2_PAL_ERR_IO;
        wifi_state.sta.ip_valid = 0u;
        types[count++] = H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP;
        break;
      }
      wifi_state.sta.ip_valid = event == WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC;
      if (wifi_state.sta.ip_valid) wifi_state.sta.ip = native_status.ip;
      types[count++] = wifi_state.sta.ip_valid
          ? H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_GOT_IP
          : H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTED;
      break;
    case WIFI_EVENT_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID:
    case WIFI_EVENT_STA_CONNECT_ASSOCIAT_FAIL:
    case WIFI_EVENT_STA_CONNECT_ASSOCIAT_TIMEOUT:
    case WIFI_EVENT_STA_NETWORK_STACK_DHCP_TIMEOUT:
      wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_FAILED;
      wifi_state.sta.disconnect_reason = (int)event;
      wifi_state.sta.ip_valid = 0u;
      types[count++] = event == WIFI_EVENT_STA_NETWORK_STACK_DHCP_TIMEOUT
          ? H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP
          : H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED;
      break;
    case WIFI_EVENT_STA_DISCONNECT:
    case WIFI_EVENT_STA_STOP:
      wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
      wifi_state.sta.ip_valid = 0u;
      if (previous_sta_state == H2_PAL_WIFI_STA_STATE_GOT_IP) {
        types[count++] = H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP;
      }
      types[count++] = H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED;
      break;
    case WIFI_EVENT_AP_START:
      wifi_state.ap.client_count = 0u;
      memset(wifi_state.ap_clients, 0, sizeof(wifi_state.ap_clients));
      wifi_state.ap.state = H2_PAL_WIFI_AP_STATE_STARTED;
      ap_type = H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STARTED;
      ap_event = 1;
      sta_changed = 0;
      break;
    case WIFI_EVENT_AP_STOP:
      wifi_state.ap.client_count = 0u;
      memset(wifi_state.ap_clients, 0, sizeof(wifi_state.ap_clients));
      wifi_state.ap.state = H2_PAL_WIFI_AP_STATE_STOPPED;
      ap_type = H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STOPPED;
      ap_event = 1;
      sta_changed = 0;
      break;
    case WIFI_EVENT_AP_ON_ASSOC:
    case WIFI_EVENT_AP_ON_DISCONNECTED: {
      sta_changed = 0;
      if (context == NULL || wifi_state.ap.state != H2_PAL_WIFI_AP_STATE_STARTED) break;
      size_t index = 0u;
      while (index < wifi_state.ap.client_count &&
             memcmp(wifi_state.ap_clients[index].mac, context, 6u) != 0) {
        ++index;
      }
      if (event == WIFI_EVENT_AP_ON_ASSOC) {
        /* SDK drift must not grow the owned cache or publish an uncached JOINED. */
        if (index == wifi_state.ap.client_count && index < H2_JIELI_AP_STATION_SLOTS) {
          h2_pal_wifi_ap_client_t *client = &wifi_state.ap_clients[index];
          memset(client, 0, sizeof(*client));
          /* The SDK event owns six MAC bytes only. As with ESP's cached
           * clients, fields absent from the event remain zero. */
          memcpy(client->mac, context, sizeof(client->mac));
          ++wifi_state.ap.client_count;
          client_event.client = *client;
          client_type = H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_JOINED;
        }
      } else if (index < wifi_state.ap.client_count) {
        client_event.client = wifi_state.ap_clients[index];
        --wifi_state.ap.client_count;
        memmove(&wifi_state.ap_clients[index], &wifi_state.ap_clients[index + 1u],
            (wifi_state.ap.client_count - index) * sizeof(wifi_state.ap_clients[0]));
        memset(&wifi_state.ap_clients[wifi_state.ap.client_count], 0,
            sizeof(wifi_state.ap_clients[0]));
        client_type = H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_LEFT;
      }
      break;
    }
    default:
      sta_changed = 0;
      break;
  }
  if (sta_changed || ap_event) ++wifi_sta_generation;
  sta_status = wifi_state.sta;
  ap_status = wifi_state.ap;
  wifi_state_unlock();
  for (size_t index = 0u; index < count; ++index) {
    post_sta_event(types[index], &sta_status);
  }
  if (ap_event) post_ap_event(ap_type, &ap_status);
  if (client_type != 0) post_system_event(client_type, &client_event, sizeof(client_event));
  wifi_state_lock();
  --wifi_callbacks_active;
  wifi_state_unlock();
  return 0;
}

static int ensure_wifi_on(void) {
  wifi_state_lock();
  const int on = wifi_state.on;
  wifi_state_unlock();
  if (on) return H2_PAL_OK;
  /* Bind events even when board startup already enabled the SDK interface. */
  wifi_set_event_callback(wifi_event);
  if (!wifi_is_on() && wifi_on() != 0) return H2_PAL_ERR_IO;
  h2_jieli_net_stack_started();
  wifi_state_lock();
  wifi_state.on = 1;
  wifi_state_unlock();
  return H2_PAL_OK;
}

static void update_sta_snapshot(h2_pal_wifi_sta_status_t *status) {
  /* SSID is owned by the PAL connect request; never borrow the SDK's mutable
   * mode-info strings from status readers. */
  wifi_get_bssid(status->bssid);
  status->bssid_set = 1u;
  status->channel = (uint8_t)wifi_get_channel();
  status->rssi = wifi_get_rssi();
  if (status->state == H2_PAL_WIFI_STA_STATE_GOT_IP) {
    h2_pal_netif_status_t netif;
    memset(&netif, 0, sizeof(netif));
    /* This callback is inside the SDK HSM: radio teardown cannot overlap.
     * IP/DNS storage still belongs to tcpip_thread, so copy there and wait. */
    status->ip_valid = h2_jieli_netif_capture_ip(&netif) == H2_PAL_OK &&
        (netif.flags & H2_PAL_NETIF_FLAG_HAS_IPV4) != 0u;
    if (status->ip_valid) {
      status->ip.ip4 = pack_ip4(
          netif.ipv4.ip[0], netif.ipv4.ip[1], netif.ipv4.ip[2], netif.ipv4.ip[3]);
      status->ip.netmask4 = pack_ip4(
          netif.netmask4.ip[0], netif.netmask4.ip[1], netif.netmask4.ip[2], netif.netmask4.ip[3]);
      status->ip.gateway4 = pack_ip4(
          netif.gateway4.ip[0], netif.gateway4.ip[1], netif.gateway4.ip[2], netif.gateway4.ip[3]);
    }
  }
}

static int sta_get_status(void *user, h2_pal_wifi_sta_status_t *out_status) {
  (void)user;
  if (out_status == NULL) return H2_PAL_ERR_INVALID_ARG;
  wifi_state_lock();
  *out_status = wifi_state.sta;
  if (!wifi_state.on) {
    memset(out_status, 0, sizeof(*out_status));
    out_status->state = H2_PAL_WIFI_STA_STATE_IDLE;
  }
  wifi_state_unlock();
  const unsigned phase = __atomic_load_n(&scan_phase, __ATOMIC_ACQUIRE);
  if (phase == SCAN_PENDING || phase == SCAN_ABANDONED) {
    /* Report the live scan without overwriting association/IP state, which
     * can continue to change through SDK events while scanning. */
    out_status->state = H2_PAL_WIFI_STA_STATE_SCANNING;
  }
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
  unsigned expected = SCAN_IDLE;
  if (!__atomic_compare_exchange_n(&scan_phase, &expected, SCAN_PENDING, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
    return H2_PAL_ERR_BUSY;
  }
  if (wifi_scan_req() != 0) {
    __atomic_store_n(&scan_phase, SCAN_IDLE, __ATOMIC_RELEASE);
    return H2_PAL_ERR_BUSY;
  }
  const uint32_t start = timer_get_ms();
  while (__atomic_load_n(&scan_phase, __ATOMIC_ACQUIRE) != SCAN_READY) {
    const uint32_t elapsed = (uint32_t)(timer_get_ms() - start);
    if (elapsed >= timeout_ms || timeout_ms - elapsed < 10u) {
      expected = SCAN_PENDING;
      if (__atomic_compare_exchange_n(&scan_phase, &expected, SCAN_ABANDONED,
                                       0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return H2_PAL_ERR_TIMEOUT;
      }
      /* Completion won the race; this caller still owns the ready result. */
      continue;
    }
    os_time_dly(1u);
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
  __atomic_store_n(&scan_phase, SCAN_IDLE, __ATOMIC_RELEASE);
  return H2_PAL_OK;
}

static int sta_connect(
    void *user, const h2_pal_wifi_sta_config_t *config,
    uint32_t timeout_ms) {
  (void)user;
  int result = h2_pal_wifi_settings_validate_sta_config(config);
  if (result != H2_PAL_OK) return result;
  char ssid[H2_PAL_WIFI_SSID_MAX + 1];
  char password[H2_PAL_WIFI_PASSWORD_MAX + 1];
  memcpy(ssid, config->ssid, config->ssid_len);
  ssid[config->ssid_len] = '\0';
  memcpy(password, config->password, config->password_len);
  password[config->password_len] = '\0';
  h2_pal_wifi_sta_status_t status;
  wifi_state_lock();
  ++wifi_sta_generation;
  wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_CONNECTING;
  wifi_state.sta.ip_valid = 0u;
  wifi_state.sta.disconnect_reason = 0;
  wifi_state.sta.ssid_len = config->ssid_len;
  memcpy(wifi_state.sta.ssid, config->ssid, config->ssid_len);
  wifi_state.sta.ssid[config->ssid_len] = '\0';
  status = wifi_state.sta;
  wifi_state_unlock();
  post_sta_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTING, &status);
  if (!wifi_is_on()) {
    /* The SDK demos install the target before wifi_on(): a placeholder-SSID
     * first HSM STA entry followed by wifi_enter_sta_mode never associates. */
    struct wifi_store_info parm = {0};
    parm.mode = STA_MODE;
    strncpy((char *)parm.ssid[0], ssid, sizeof(parm.ssid[0]) - 1u);
    strncpy((char *)parm.pwd[0], password, sizeof(parm.pwd[0]) - 1u);
    parm.connect_best_network = 0;
    wifi_set_sta_connect_timeout(timeout_ms == 0u
        ? 30 : (int)(timeout_ms / 1000u + (timeout_ms % 1000u != 0u)));
    result = wifi_set_default_mode(
        &parm, 1 /* Force this mode after wifi_on. */,
        0 /* PAL settings own persistence; do not store in the SDK. */);
    memset(&parm, 0, sizeof(parm));
    if (result != 0) {
      wifi_state_lock();
      ++wifi_sta_generation;
      wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_FAILED;
      wifi_state.sta.ip_valid = 0u;
      wifi_state.sta.disconnect_reason = result;
      status = wifi_state.sta;
      wifi_state_unlock();
      post_sta_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED, &status);
      return H2_PAL_ERR_IO;
    }
    result = ensure_wifi_on();
    if (result != H2_PAL_OK) return result;
  } else {
    result = ensure_wifi_on();
    if (result != H2_PAL_OK) return result;
    wifi_set_sta_connect_timeout(timeout_ms == 0u
        ? 30 : (int)(timeout_ms / 1000u + (timeout_ms % 1000u != 0u)));
    /* With network_connect_block == 0, the SDK queues the credentials and its
     * tail returns wifi_sta_connect_state != 5 ? -1 : 0 without waiting. This
     * only reflects whether STA was already connected, not whether starting
     * the connection failed. SDK events and the PAL budget decide the outcome. */
    (void)wifi_enter_sta_mode(ssid, password);
  }
  if (timeout_ms == 0u) return H2_PAL_OK;
  const uint32_t started = timer_get_ms();
  for (;;) {
    wifi_state_lock();
    const h2_pal_wifi_sta_state_t state = wifi_state.sta.state;
    wifi_state_unlock();
    if (state == H2_PAL_WIFI_STA_STATE_GOT_IP) break;
    if (state == H2_PAL_WIFI_STA_STATE_FAILED) {
      return H2_PAL_ERR_IO;
    }
    if ((uint32_t)(timer_get_ms() - started) >= timeout_ms) {
      return H2_PAL_ERR_TIMEOUT;
    }
    os_time_dly(1u);
  }
  return H2_PAL_OK;
}

static int wifi_stop(void) {
  wifi_state_lock();
  const int on = wifi_state.on;
  wifi_state_unlock();
  if (!on && !wifi_is_on()) return H2_PAL_OK;
  h2_jieli_net_stack_stopping();
  if (wifi_off() != 0) return H2_PAL_ERR_IO;
  h2_jieli_net_stack_stopped();
  wifi_state_lock();
  ++wifi_sta_generation;
  wifi_state.on = 0;
  wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
  wifi_state.sta.ip_valid = 0u;
  wifi_state.ap.state = H2_PAL_WIFI_AP_STATE_STOPPED;
  wifi_state.ap.client_count = 0u;
  memset(wifi_state.ap_clients, 0, sizeof(wifi_state.ap_clients));
  wifi_state_unlock();
  return H2_PAL_OK;
}

static int sta_disconnect(void *user) {
  (void)user;
  if (!wifi_is_on()) return H2_PAL_OK;
  /* wifi_off() is irreversible for STA use on this SDK: wifi_on() re-adds
   * the retained lwIP netif and asserts "netif already added". Leave STA
   * through config/monitor mode instead, keeping the radio and lwIP up. */
  /* Like wifi_enter_sta_mode, this only posts an asynchronous HSM message;
   * its return value does not report completion of the mode transition. */
  (void)wifi_enter_smp_cfg_mode();
  h2_pal_wifi_sta_status_t status;
  wifi_state_lock();
  const int had_ip = wifi_state.sta.state == H2_PAL_WIFI_STA_STATE_GOT_IP;
  ++wifi_sta_generation;
  wifi_state.sta.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
  wifi_state.sta.ip_valid = 0u;
  status = wifi_state.sta;
  wifi_state_unlock();
  if (had_ip) {
    post_sta_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_LOST_IP, &status);
  }
  post_sta_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED, &status);
  return H2_PAL_OK;
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
  /* wifi_conf.c bounds MaxStaNum at 5; its wl_set_passphrase selects
   * OPEN/NONE or WPA2PSK/AES, not an arbitrary requested authentication mode. */
  if (config->max_clients > 5u ||
      (config->security != H2_PAL_WIFI_SECURITY_OPEN &&
       config->security != H2_PAL_WIFI_SECURITY_WPA2)) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  if (config->security == H2_PAL_WIFI_SECURITY_OPEN &&
      config->password_len != 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (config->channel > 14u) return H2_PAL_ERR_INVALID_ARG;
  const uint8_t channel = config->channel != 0u ? config->channel : 1u;
  const uint8_t max_clients = config->max_clients != 0u ? config->max_clients : 2u;
  result = wifi_stop();
  if (result != H2_PAL_OK) return result;
  if (h2_jieli_wifi_configure_ap(channel, max_clients, config->hidden != 0u) != 0) {
    return H2_PAL_ERR_IO;
  }
  result = ensure_wifi_on();
  if (result != H2_PAL_OK) return result;
  char ssid[H2_PAL_WIFI_SSID_MAX + 1];
  char password[H2_PAL_WIFI_PASSWORD_MAX + 1];
  memcpy(ssid, config->ssid, config->ssid_len);
  ssid[config->ssid_len] = '\0';
  memcpy(password, config->password, config->password_len);
  password[config->password_len] = '\0';
  wifi_state_lock();
  wifi_state.ap.state = H2_PAL_WIFI_AP_STATE_STARTING;
  wifi_state.ap.max_clients = max_clients;
  wifi_state.ap.security = config->security;
  wifi_state.ap.hidden = config->hidden != 0u;
  wifi_state.ap.ssid_len = config->ssid_len;
  memcpy(wifi_state.ap.ssid, config->ssid, config->ssid_len);
  wifi_state.ap.ssid[config->ssid_len] = '\0';
  wifi_state_unlock();
  if (wifi_enter_ap_mode(ssid, password) != 0) {
    /* A mode-switch failure does not prove the SDK stopped its previous AP. */
    wifi_state_lock();
    wifi_state.ap.state = H2_PAL_WIFI_AP_STATE_UNKNOWN;
    wifi_state_unlock();
    return H2_PAL_ERR_IO;
  }
  const uint32_t started = timer_get_ms();
  for (;;) {
    wifi_state_lock();
    const h2_pal_wifi_ap_state_t state = wifi_state.ap.state;
    wifi_state_unlock();
    if (state == H2_PAL_WIFI_AP_STATE_STARTED) break;
    if ((uint32_t)(timer_get_ms() - started) >= timeout_ms) {
      return H2_PAL_ERR_TIMEOUT;
    }
    os_time_dly(1u);
  }
  const uint8_t actual_channel = (uint8_t)wifi_get_channel();
  wifi_state_lock();
  wifi_state.ap.channel = actual_channel;
  wifi_state_unlock();
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
  wifi_state_lock();
  *out_status = wifi_state.ap;
  wifi_state_unlock();
  return H2_PAL_OK;
}

static int ap_get_clients(
    void *user, h2_pal_wifi_ap_client_t *out_clients, size_t max_clients,
    size_t *out_count) {
  (void)user;
  if (out_count == NULL || (out_clients == NULL && max_clients != 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  wifi_state_lock();
  const size_t count = wifi_state.ap.state == H2_PAL_WIFI_AP_STATE_STARTED
      ? wifi_state.ap.client_count : 0u;
  *out_count = count < max_clients ? count : max_clients;
  if (*out_count != 0u) {
    memcpy(out_clients, wifi_state.ap_clients, *out_count * sizeof(*out_clients));
  }
  wifi_state_unlock();
  return H2_PAL_OK;
}

/* Serialize task-side radio mutations, including reentrant scan callbacks.
 * A timed-out scan still owns SDK storage until its completion callback. */
static unsigned wifi_operation_busy;

static int wifi_operation_begin(void) {
  /* SDK event subscribers run on the network HSM task. A synchronous SDK
   * request there would wait for the callback itself to release its mutex.
   * Reject admission while callbacks dispatch; never wait for subscribers. */
  wifi_state_lock();
  const int in_callback = wifi_callbacks_active != 0u;
  wifi_state_unlock();
  if (in_callback) return H2_PAL_ERR_BUSY;
  if (__atomic_exchange_n(&wifi_operation_busy, 1u, __ATOMIC_ACQUIRE)) {
    return H2_PAL_ERR_BUSY;
  }
  scan_reap_completed();
  if (__atomic_load_n(&scan_phase, __ATOMIC_ACQUIRE) != SCAN_IDLE) {
    __atomic_store_n(&wifi_operation_busy, 0u, __ATOMIC_RELEASE);
    return H2_PAL_ERR_BUSY;
  }
  return H2_PAL_OK;
}

int h2_jieli_wifi_netif_begin(h2_pal_netif_status_t *status, uint32_t *generation) {
  int result = wifi_operation_begin();
  if (result != H2_PAL_OK) return result;
  memset(status, 0, sizeof(*status));
  wifi_state_lock();
  *generation = wifi_sta_generation;
  status->kind = wifi_state.ap.state == H2_PAL_WIFI_AP_STATE_STARTED ||
      wifi_state.ap.state == H2_PAL_WIFI_AP_STATE_STARTING
      ? H2_PAL_NETIF_KIND_WIFI_AP : H2_PAL_NETIF_KIND_WIFI_STA;
  status->ref.kind = status->kind;
  status->ref.type = H2_PAL_NETIF_REF_NAME;
  strcpy(status->ref.name, "wl0");
  status->mtu = 1500u;
  if (wifi_state.on) {
    status->flags = H2_PAL_NETIF_FLAG_UP;
    if (status->kind == H2_PAL_NETIF_KIND_WIFI_AP) {
      if (wifi_state.ap.state == H2_PAL_WIFI_AP_STATE_STARTED) {
        status->flags |= H2_PAL_NETIF_FLAG_LINK_UP | H2_PAL_NETIF_FLAG_HAS_IPV4;
      }
    } else {
      if (wifi_state.sta.state == H2_PAL_WIFI_STA_STATE_CONNECTED ||
          wifi_state.sta.state == H2_PAL_WIFI_STA_STATE_GOT_IP) {
        status->flags |= H2_PAL_NETIF_FLAG_LINK_UP;
      }
      if (wifi_state.sta.ip_valid) status->flags |= H2_PAL_NETIF_FLAG_HAS_IPV4;
    }
  }
  wifi_state_unlock();
  return H2_PAL_OK;
}

int h2_jieli_wifi_netif_end(uint32_t generation) {
  wifi_state_lock();
  const int unchanged = generation == wifi_sta_generation;
  wifi_state_unlock();
  __atomic_store_n(&wifi_operation_busy, 0u, __ATOMIC_RELEASE);
  return unchanged ? H2_PAL_OK : H2_PAL_ERR_BUSY;
}

static int guarded_sta_scan(void *user, const h2_pal_wifi_scan_request_t *request,
    h2_pal_wifi_scan_result_fn on_result, void *callback_user, uint32_t timeout_ms) {
  int result = wifi_operation_begin();
  if (result != H2_PAL_OK) return result;
  result = sta_scan(user, request, on_result, callback_user, timeout_ms);
  __atomic_store_n(&wifi_operation_busy, 0u, __ATOMIC_RELEASE);
  return result;
}

static int guarded_sta_connect(void *user, const h2_pal_wifi_sta_config_t *config, uint32_t timeout_ms) {
  int result = wifi_operation_begin();
  if (result != H2_PAL_OK) return result;
  result = sta_connect(user, config, timeout_ms);
  __atomic_store_n(&wifi_operation_busy, 0u, __ATOMIC_RELEASE);
  return result;
}

static int guarded_sta_disconnect(void *user) {
  int result = wifi_operation_begin();
  if (result != H2_PAL_OK) return result;
  result = sta_disconnect(user);
  __atomic_store_n(&wifi_operation_busy, 0u, __ATOMIC_RELEASE);
  return result;
}

static int guarded_ap_start(void *user, const h2_pal_wifi_ap_config_t *config, uint32_t timeout_ms) {
  int result = wifi_operation_begin();
  if (result != H2_PAL_OK) return result;
  result = ap_start(user, config, timeout_ms);
  __atomic_store_n(&wifi_operation_busy, 0u, __ATOMIC_RELEASE);
  return result;
}

static int guarded_ap_stop(void *user, uint32_t timeout_ms) {
  int result = wifi_operation_begin();
  if (result != H2_PAL_OK) return result;
  result = ap_stop(user, timeout_ms);
  __atomic_store_n(&wifi_operation_busy, 0u, __ATOMIC_RELEASE);
  return result;
}

static int guarded_wifi_get_mac(void *user, uint8_t out_mac[6]) {
  int result = wifi_operation_begin();
  if (result != H2_PAL_OK) return result;
  result = wifi_get_mac_address(user, out_mac);
  __atomic_store_n(&wifi_operation_busy, 0u, __ATOMIC_RELEASE);
  return result;
}

static int sta_connect_and_save(void *user,
    const h2_pal_wifi_sta_config_t *config, uint32_t timeout_ms) {
  int result = wifi_operation_begin();
  if (result != H2_PAL_OK) return result;
  static const h2_pal_wifi_sta_vtable_t raw_vtable = {
      .get_status = sta_get_status, .connect = sta_connect, .disconnect = sta_disconnect,
  };
  const h2_pal_wifi_sta_api_t raw = {user, &raw_vtable};
  const h2_wifi_sta_dependencies_t deps = {
      .sta = &raw,
      .settings = h2_jieli_ac791n_devkit_wifi_settings_api(),
      .time = h2_jieli_wl82_platform_time_api(),
  };
  result = h2_wifi_sta_connect_and_save(&deps, config, timeout_ms);
  __atomic_store_n(&wifi_operation_busy, 0u, __ATOMIC_RELEASE);
  return result;
}

const h2_pal_wifi_sta_api_t *h2_jieli_ac791n_devkit_wifi_sta_api(void) {
  static const h2_pal_wifi_sta_vtable_t vtable = {
      .get_status = sta_get_status,
      .scan = guarded_sta_scan,
      .connect = guarded_sta_connect,
      .connect_and_save = sta_connect_and_save,
      .disconnect = guarded_sta_disconnect,
      .get_mac = guarded_wifi_get_mac,
  };
  static const h2_pal_wifi_sta_api_t api = {.user = NULL, .vtable = &vtable};
  return &api;
}

const h2_pal_wifi_ap_api_t *h2_jieli_ac791n_devkit_wifi_ap_api(void) {
  static const h2_pal_wifi_ap_vtable_t vtable = {
      .start = guarded_ap_start,
      .stop = guarded_ap_stop,
      .get_status = ap_get_status,
      .get_clients = ap_get_clients,
      .get_mac = guarded_wifi_get_mac,
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
