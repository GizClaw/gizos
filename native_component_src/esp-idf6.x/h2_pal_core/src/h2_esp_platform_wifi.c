#include "h2_esp_platform_core.h"
#include "h2_esp_platform_safe_call.h"
#include "h2_esp_platform_wifi_activity.h"
#include "h2_esp_platform_wifi_internal.h"
#include "h2_esp_wifi_teardown.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/def.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <string.h>

static esp_netif_t *s_h2_esp_wifi_sta_netif;
static EventGroupHandle_t s_h2_esp_wifi_events;
static int s_h2_esp_wifi_started;
static int s_h2_esp_wifi_events_registered;
static int s_h2_esp_wifi_sta_disconnect_reason;
static int s_h2_esp_wifi_sta_reconnect_enabled;
static uint32_t s_h2_esp_wifi_sta_reconnect_attempts;
static portMUX_TYPE s_h2_esp_wifi_sta_reconnect_lock =
    portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_h2_esp_wifi_safe_mutex_storage;
static SemaphoreHandle_t s_h2_esp_wifi_safe_mutex;
static portMUX_TYPE s_h2_esp_wifi_safe_mutex_init_lock =
    portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_h2_esp_wifi_activity_lock = portMUX_INITIALIZER_UNLOCKED;
static h2_esp_platform_wifi_activity_fn s_h2_esp_wifi_activity_callback;
static void *s_h2_esp_wifi_activity_user;
static int s_h2_esp_wifi_activity_active;
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
static esp_netif_t *s_h2_esp_wifi_ap_netif;
static portMUX_TYPE s_h2_esp_wifi_ap_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_h2_esp_wifi_ap_active;
static h2_pal_wifi_ap_status_t s_h2_esp_wifi_ap_status;
static h2_pal_wifi_ap_config_t s_h2_esp_wifi_ap_config;
static h2_pal_wifi_ap_client_t
    s_h2_esp_wifi_ap_clients[H2_PAL_WIFI_AP_MAX_CLIENTS];
static size_t s_h2_esp_wifi_ap_client_count;
#endif

#define H2_ESP_WIFI_EVENT_CONNECTED BIT0
#define H2_ESP_WIFI_EVENT_DISCONNECTED BIT1
#define H2_ESP_WIFI_EVENT_GOT_IP BIT2
#define H2_ESP_WIFI_SAFE_STACK_DEPTH 4096u
#define H2_ESP_WIFI_STA_RECONNECT_ATTEMPTS 5u

typedef enum h2_esp_wifi_safe_op {
  H2_ESP_WIFI_SAFE_NVS_INIT = 1,
  H2_ESP_WIFI_SAFE_INIT,
  H2_ESP_WIFI_SAFE_SET_CONFIG,
} h2_esp_wifi_safe_op_t;

typedef struct h2_esp_wifi_safe_call {
  h2_esp_wifi_safe_op_t op;
  wifi_interface_t interface;
  wifi_init_config_t init_config;
  wifi_config_t config;
  esp_err_t result;
} h2_esp_wifi_safe_call_t;

static size_t h2_esp_wifi_strnlen(const uint8_t *value, size_t max_len);
static int h2_esp_wifi_map_error(esp_err_t err);

static void h2_esp_wifi_set_activity(int active) {
  h2_esp_platform_wifi_activity_fn callback = NULL;
  void *user = NULL;
  portENTER_CRITICAL(&s_h2_esp_wifi_activity_lock);
  active = active != 0;
  if (s_h2_esp_wifi_activity_active != active) {
    s_h2_esp_wifi_activity_active = active;
    callback = s_h2_esp_wifi_activity_callback;
    user = s_h2_esp_wifi_activity_user;
  }
  portEXIT_CRITICAL(&s_h2_esp_wifi_activity_lock);
  if (callback != NULL) {
    callback(user, active != 0);
  }
}

void h2_esp_platform_wifi_set_activity_observer(
    h2_esp_platform_wifi_activity_fn callback, void *user) {
  int active;
  portENTER_CRITICAL(&s_h2_esp_wifi_activity_lock);
  s_h2_esp_wifi_activity_callback = callback;
  s_h2_esp_wifi_activity_user = callback != NULL ? user : NULL;
  active = s_h2_esp_wifi_activity_active;
  portEXIT_CRITICAL(&s_h2_esp_wifi_activity_lock);
  if (callback != NULL) {
    callback(user, active != 0);
  }
}

static void h2_esp_wifi_sta_reconnect_disable(void) {
  portENTER_CRITICAL(&s_h2_esp_wifi_sta_reconnect_lock);
  s_h2_esp_wifi_sta_reconnect_enabled = 0;
  s_h2_esp_wifi_sta_reconnect_attempts = 0u;
  portEXIT_CRITICAL(&s_h2_esp_wifi_sta_reconnect_lock);
}

static void h2_esp_wifi_sta_reconnect_enable(void) {
  portENTER_CRITICAL(&s_h2_esp_wifi_sta_reconnect_lock);
  s_h2_esp_wifi_sta_reconnect_enabled = 1;
  s_h2_esp_wifi_sta_reconnect_attempts = 0u;
  portEXIT_CRITICAL(&s_h2_esp_wifi_sta_reconnect_lock);
}

static void h2_esp_wifi_sta_reconnect_reset_attempts(void) {
  portENTER_CRITICAL(&s_h2_esp_wifi_sta_reconnect_lock);
  s_h2_esp_wifi_sta_reconnect_attempts = 0u;
  portEXIT_CRITICAL(&s_h2_esp_wifi_sta_reconnect_lock);
}

static int h2_esp_wifi_sta_reconnect_is_enabled(void) {
  int enabled;
  portENTER_CRITICAL(&s_h2_esp_wifi_sta_reconnect_lock);
  enabled = s_h2_esp_wifi_sta_reconnect_enabled;
  portEXIT_CRITICAL(&s_h2_esp_wifi_sta_reconnect_lock);
  return enabled;
}

static int h2_esp_wifi_sta_reconnect_claim_attempt(void) {
  int reconnect = 0;
  portENTER_CRITICAL(&s_h2_esp_wifi_sta_reconnect_lock);
  if (s_h2_esp_wifi_sta_reconnect_enabled != 0 &&
      s_h2_esp_wifi_sta_reconnect_attempts <
          H2_ESP_WIFI_STA_RECONNECT_ATTEMPTS) {
    ++s_h2_esp_wifi_sta_reconnect_attempts;
    reconnect = 1;
  }
  portEXIT_CRITICAL(&s_h2_esp_wifi_sta_reconnect_lock);
  return reconnect;
}

static SemaphoreHandle_t h2_esp_wifi_safe_mutex(void) {
  portENTER_CRITICAL(&s_h2_esp_wifi_safe_mutex_init_lock);
  if (s_h2_esp_wifi_safe_mutex == NULL) {
    s_h2_esp_wifi_safe_mutex =
        xSemaphoreCreateMutexStatic(&s_h2_esp_wifi_safe_mutex_storage);
  }
  portEXIT_CRITICAL(&s_h2_esp_wifi_safe_mutex_init_lock);
  return s_h2_esp_wifi_safe_mutex;
}

static void IRAM_ATTR h2_esp_wifi_safe_callback(void *context) {
  h2_esp_wifi_safe_call_t *call = (h2_esp_wifi_safe_call_t *)context;
  if (call->op == H2_ESP_WIFI_SAFE_NVS_INIT) {
    call->result = nvs_flash_init();
  } else if (call->op == H2_ESP_WIFI_SAFE_INIT) {
    call->result = esp_wifi_init(&call->init_config);
  } else if (call->op == H2_ESP_WIFI_SAFE_SET_CONFIG) {
    call->result = esp_wifi_set_config(call->interface, &call->config);
  } else {
    call->result = ESP_ERR_INVALID_ARG;
  }
}

static esp_err_t h2_esp_wifi_run_safe(h2_esp_wifi_safe_call_t *call) {
  SemaphoreHandle_t mutex = h2_esp_wifi_safe_mutex();
  h2_pal_result_t rc;
  if (mutex == NULL || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
    return ESP_ERR_NO_MEM;
  }
  rc = h2_esp_platform_safe_call(h2_esp_wifi_safe_callback, call, sizeof(*call),
                                 H2_ESP_WIFI_SAFE_STACK_DEPTH);
  (void)xSemaphoreGive(mutex);
  return rc == H2_PAL_OK ? call->result : ESP_ERR_NO_MEM;
}

int h2_esp_platform_wifi_set_config_safe(wifi_interface_t interface,
                                         const wifi_config_t *config) {
  if (config == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_esp_wifi_safe_call_t call = {
      .op = H2_ESP_WIFI_SAFE_SET_CONFIG,
      .interface = interface,
      .config = *config,
  };
  return h2_esp_wifi_map_error(h2_esp_wifi_run_safe(&call));
}

static void h2_esp_wifi_post_system_event_payload(
    h2_pal_system_event_type_t type, const void *payload, size_t payload_size) {
  const h2_pal_system_event_api_t *api = h2_esp_platform_system_event_api();
  if (api == NULL) {
    return;
  }

  h2_pal_system_event_t event;
  memset(&event, 0, sizeof(event));
  event.type = type;
  if (payload != NULL && payload_size > 0u) {
    event.payload = payload;
    event.payload_size = payload_size;
  }
  (void)h2_pal_system_event_post(api, &event, 0u);
}

static void
h2_esp_wifi_post_sta_system_event(h2_pal_system_event_type_t type,
                                  const h2_pal_wifi_sta_status_t *status) {
  h2_esp_wifi_post_system_event_payload(type, status,
                                        status != NULL ? sizeof(*status) : 0u);
}

#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
static void
h2_esp_wifi_post_ap_system_event(h2_pal_system_event_type_t type,
                                 const h2_pal_wifi_ap_status_t *status) {
  h2_pal_wifi_ap_event_t event;
  memset(&event, 0, sizeof(event));
  if (status != NULL) {
    event.status = *status;
  }
  h2_esp_wifi_post_system_event_payload(type, &event, sizeof(event));
}

static void
h2_esp_wifi_post_ap_client_system_event(h2_pal_system_event_type_t type,
                                        const h2_pal_wifi_ap_client_t *client) {
  h2_pal_wifi_ap_client_event_t event;
  memset(&event, 0, sizeof(event));
  if (client != NULL) {
    event.client = *client;
  }
  h2_esp_wifi_post_system_event_payload(type, &event, sizeof(event));
}

static h2_pal_wifi_ap_client_t *
h2_esp_wifi_find_ap_client(const uint8_t mac[6]) {
  if (mac == NULL) {
    return NULL;
  }
  for (size_t i = 0u; i < s_h2_esp_wifi_ap_client_count; ++i) {
    if (memcmp(s_h2_esp_wifi_ap_clients[i].mac, mac, 6u) == 0) {
      return &s_h2_esp_wifi_ap_clients[i];
    }
  }
  return NULL;
}

static h2_pal_wifi_ap_client_t *
h2_esp_wifi_upsert_ap_client(const uint8_t mac[6]) {
  h2_pal_wifi_ap_client_t *client = h2_esp_wifi_find_ap_client(mac);
  if (client != NULL) {
    return client;
  }
  if (s_h2_esp_wifi_ap_client_count >= H2_PAL_WIFI_AP_MAX_CLIENTS) {
    return NULL;
  }
  client = &s_h2_esp_wifi_ap_clients[s_h2_esp_wifi_ap_client_count++];
  memset(client, 0, sizeof(*client));
  memcpy(client->mac, mac, 6u);
  return client;
}

static h2_pal_wifi_ap_client_t
h2_esp_wifi_remove_ap_client(const uint8_t mac[6]) {
  h2_pal_wifi_ap_client_t removed;
  memset(&removed, 0, sizeof(removed));
  for (size_t i = 0u; i < s_h2_esp_wifi_ap_client_count; ++i) {
    if (memcmp(s_h2_esp_wifi_ap_clients[i].mac, mac, 6u) == 0) {
      removed = s_h2_esp_wifi_ap_clients[i];
      if (i + 1u < s_h2_esp_wifi_ap_client_count) {
        memmove(&s_h2_esp_wifi_ap_clients[i], &s_h2_esp_wifi_ap_clients[i + 1u],
                (s_h2_esp_wifi_ap_client_count - i - 1u) *
                    sizeof(s_h2_esp_wifi_ap_clients[0]));
      }
      s_h2_esp_wifi_ap_client_count--;
      break;
    }
  }
  return removed;
}
#endif

static int h2_esp_wifi_teardown_dhcp_stop(void *user) {
  (void)user;
  if (s_h2_esp_wifi_sta_netif == NULL) {
    return ESP_OK;
  }

  /* DHCP stop may transmit RELEASE from the TCP/IP task. Complete it while
   * the Wi-Fi driver still owns a valid transmit callback. */
  return esp_netif_dhcpc_stop(s_h2_esp_wifi_sta_netif);
}

static int h2_esp_wifi_teardown_stop(void *user) {
  (void)user;
  return esp_wifi_stop();
}

static int h2_esp_wifi_teardown_deinit(void *user) {
  (void)user;
  return esp_wifi_deinit();
}

static int h2_esp_wifi_teardown_map_error(void *user, int error) {
  (void)user;
  return h2_esp_wifi_map_error((esp_err_t)error);
}

static int h2_esp_wifi_teardown_driver(void) {
  const h2_esp_wifi_teardown_config_t config = {
      .dhcp_stop = h2_esp_wifi_teardown_dhcp_stop,
      .wifi_stop = h2_esp_wifi_teardown_stop,
      .wifi_deinit = h2_esp_wifi_teardown_deinit,
      .map_error = h2_esp_wifi_teardown_map_error,
      .success = ESP_OK,
      .dhcp_already_stopped = ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED,
      .wifi_not_initialized = ESP_ERR_WIFI_NOT_INIT,
      .wifi_not_started = ESP_ERR_WIFI_NOT_STARTED,
  };
  return h2_esp_wifi_run_driver_teardown(&config);
}

static int h2_esp_wifi_stop_driver_if_sta_idle(void) {
  if (s_h2_esp_wifi_events != NULL) {
    EventBits_t bits = xEventGroupGetBits(s_h2_esp_wifi_events);
    if ((bits & (H2_ESP_WIFI_EVENT_CONNECTED | H2_ESP_WIFI_EVENT_GOT_IP)) !=
        0u) {
      return H2_PAL_OK;
    }
  }

  wifi_ap_record_t ap_info;
  memset(&ap_info, 0, sizeof(ap_info));
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    return H2_PAL_OK;
  }

  h2_esp_wifi_sta_reconnect_disable();
  int rc = h2_esp_wifi_teardown_driver();
  if (rc != H2_PAL_OK) {
    return rc;
  }

  if (s_h2_esp_wifi_sta_netif != NULL) {
    esp_netif_destroy_default_wifi(s_h2_esp_wifi_sta_netif);
    s_h2_esp_wifi_sta_netif = NULL;
    (void)h2_esp_platform_netif_reconcile_default();
  }
  s_h2_esp_wifi_started = 0;
  if (s_h2_esp_wifi_events != NULL) {
    xEventGroupClearBits(s_h2_esp_wifi_events, H2_ESP_WIFI_EVENT_CONNECTED |
                                                   H2_ESP_WIFI_EVENT_GOT_IP);
    xEventGroupSetBits(s_h2_esp_wifi_events, H2_ESP_WIFI_EVENT_DISCONNECTED);
  }
  return H2_PAL_OK;
}

static void h2_esp_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data) {
  (void)arg;
  if (s_h2_esp_wifi_events == NULL) {
    return;
  }
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
    h2_pal_wifi_sta_status_t status;
    memset(&status, 0, sizeof(status));
    status.state = H2_PAL_WIFI_STA_STATE_CONNECTED;
    s_h2_esp_wifi_sta_disconnect_reason = 0;
    h2_esp_wifi_sta_reconnect_reset_attempts();
    const wifi_event_sta_connected_t *connected =
        (const wifi_event_sta_connected_t *)event_data;
    if (connected != NULL) {
      status.ssid_len =
          h2_esp_wifi_strnlen(connected->ssid, H2_PAL_WIFI_SSID_MAX);
      memcpy(status.ssid, connected->ssid, status.ssid_len);
      status.ssid[status.ssid_len] = '\0';
      memcpy(status.bssid, connected->bssid, sizeof(status.bssid));
      status.bssid_set = 1u;
      status.channel = connected->channel;
    }
    xEventGroupClearBits(s_h2_esp_wifi_events, H2_ESP_WIFI_EVENT_DISCONNECTED);
    xEventGroupSetBits(s_h2_esp_wifi_events, H2_ESP_WIFI_EVENT_CONNECTED);
    h2_esp_wifi_post_sta_system_event(
        H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTED, &status);
    return;
  }
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    h2_pal_wifi_sta_status_t status;
    memset(&status, 0, sizeof(status));
    status.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
    const wifi_event_sta_disconnected_t *disconnected =
        (const wifi_event_sta_disconnected_t *)event_data;
    if (disconnected != NULL) {
      status.disconnect_reason = disconnected->reason;
      s_h2_esp_wifi_sta_disconnect_reason = disconnected->reason;
    }
    xEventGroupClearBits(s_h2_esp_wifi_events, H2_ESP_WIFI_EVENT_CONNECTED |
                                                   H2_ESP_WIFI_EVENT_GOT_IP);
    xEventGroupSetBits(s_h2_esp_wifi_events, H2_ESP_WIFI_EVENT_DISCONNECTED);
    h2_esp_wifi_post_sta_system_event(
        H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_DISCONNECTED, &status);
    if (h2_esp_wifi_sta_reconnect_claim_attempt()) {
      (void)esp_wifi_connect();
    }
    (void)h2_esp_platform_netif_reconcile_default();
    return;
  }
  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(s_h2_esp_wifi_events, H2_ESP_WIFI_EVENT_GOT_IP);
    h2_pal_wifi_sta_status_t status;
    memset(&status, 0, sizeof(status));
    status.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
    const ip_event_got_ip_t *got_ip = (const ip_event_got_ip_t *)event_data;
    if (got_ip != NULL) {
      status.ip.ip4 = lwip_ntohl(got_ip->ip_info.ip.addr);
      status.ip.netmask4 = lwip_ntohl(got_ip->ip_info.netmask.addr);
      status.ip.gateway4 = lwip_ntohl(got_ip->ip_info.gw.addr);
      status.ip_valid = 1u;
    }
    h2_esp_wifi_post_sta_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_GOT_IP,
                                      &status);
    (void)h2_esp_platform_netif_reconcile_default();
    return;
  }
  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
    (void)h2_esp_platform_netif_reconcile_default();
    return;
  }
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
    return;
  }
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
    return;
  }
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
    const wifi_event_ap_staconnected_t *connected =
        (const wifi_event_ap_staconnected_t *)event_data;
    if (connected != NULL) {
      h2_pal_wifi_ap_client_t event_client;
      memset(&event_client, 0, sizeof(event_client));
      int have_client = 0;
      portENTER_CRITICAL(&s_h2_esp_wifi_ap_lock);
      h2_pal_wifi_ap_client_t *client =
          h2_esp_wifi_upsert_ap_client(connected->mac);
      if (client != NULL) {
        client->station_id = connected->aid;
        s_h2_esp_wifi_ap_status.client_count = s_h2_esp_wifi_ap_client_count;
        event_client = *client;
        have_client = 1;
      }
      portEXIT_CRITICAL(&s_h2_esp_wifi_ap_lock);
      if (have_client != 0) {
        h2_esp_wifi_post_ap_client_system_event(
            H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_JOINED, &event_client);
      }
    }
    return;
  }
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    const wifi_event_ap_stadisconnected_t *disconnected =
        (const wifi_event_ap_stadisconnected_t *)event_data;
    if (disconnected != NULL) {
      portENTER_CRITICAL(&s_h2_esp_wifi_ap_lock);
      h2_pal_wifi_ap_client_t client =
          h2_esp_wifi_remove_ap_client(disconnected->mac);
      if (client.station_id == 0) {
        memcpy(client.mac, disconnected->mac, 6u);
        client.station_id = disconnected->aid;
      }
      s_h2_esp_wifi_ap_status.client_count = s_h2_esp_wifi_ap_client_count;
      portEXIT_CRITICAL(&s_h2_esp_wifi_ap_lock);
      h2_esp_wifi_post_ap_client_system_event(
          H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_CLIENT_LEFT, &client);
      if (client.lease_valid != 0u) {
        h2_esp_wifi_post_ap_client_system_event(
            H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_LEASE_RELEASED, &client);
      }
    }
    return;
  }
  if (event_base == IP_EVENT && event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
    const ip_event_assigned_ip_to_client_t *assigned =
        (const ip_event_assigned_ip_to_client_t *)event_data;
    if (assigned != NULL) {
      h2_pal_wifi_ap_client_t event_client;
      memset(&event_client, 0, sizeof(event_client));
      int have_client = 0;
      portENTER_CRITICAL(&s_h2_esp_wifi_ap_lock);
      h2_pal_wifi_ap_client_t *client =
          h2_esp_wifi_upsert_ap_client(assigned->mac);
      if (client != NULL) {
        client->lease.ip4 = lwip_ntohl(assigned->ip.addr);
        client->lease_valid = 1u;
        s_h2_esp_wifi_ap_status.client_count = s_h2_esp_wifi_ap_client_count;
        event_client = *client;
        have_client = 1;
      }
      portEXIT_CRITICAL(&s_h2_esp_wifi_ap_lock);
      if (have_client != 0) {
        h2_esp_wifi_post_ap_client_system_event(
            H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_LEASE_GRANTED, &event_client);
      }
    }
    return;
  }
#endif
}

static size_t h2_esp_wifi_strnlen(const uint8_t *value, size_t max_len) {
  size_t len = 0u;
  while (len < max_len && value[len] != 0u) {
    len++;
  }
  return len;
}

static int h2_esp_wifi_map_error(esp_err_t err) {
  switch (err) {
  case ESP_OK:
    return H2_PAL_OK;
  case ESP_ERR_INVALID_ARG:
  case ESP_ERR_WIFI_MAC:
  case ESP_ERR_WIFI_SSID:
  case ESP_ERR_WIFI_PASSWORD:
    return H2_PAL_ERR_INVALID_ARG;
  case ESP_ERR_WIFI_NOT_INIT:
  case ESP_ERR_WIFI_NOT_STARTED:
  case ESP_ERR_NVS_NOT_INITIALIZED:
    return H2_PAL_ERR_UNAVAILABLE;
  case ESP_ERR_NOT_SUPPORTED:
  case ESP_ERR_WIFI_IF:
    return H2_PAL_ERR_UNSUPPORTED;
  case ESP_ERR_WIFI_STATE:
  case ESP_ERR_WIFI_INIT_STATE:
  case ESP_ERR_INVALID_STATE:
  case ESP_ERR_WIFI_MODE:
  case ESP_ERR_WIFI_CONN:
    return H2_PAL_ERR_INVALID_STATE;
  case ESP_ERR_NO_MEM:
    return H2_PAL_ERR_NO_MEMORY;
  case ESP_ERR_WIFI_TIMEOUT:
    return H2_PAL_ERR_TIMEOUT;
  case ESP_ERR_WIFI_NOT_CONNECT:
  case ESP_ERR_WIFI_NOT_ASSOC:
    return H2_PAL_ERR_NOT_FOUND;
  case ESP_ERR_WIFI_NVS:
  default:
    return H2_PAL_ERR_IO;
  }
}

int h2_esp_platform_wifi_ensure_started(void) {
  if (s_h2_esp_wifi_started != 0) {
    return H2_PAL_OK;
  }

  h2_esp_wifi_safe_call_t safe_call = {
      .op = H2_ESP_WIFI_SAFE_NVS_INIT,
  };
  esp_err_t err = h2_esp_wifi_run_safe(&safe_call);
  if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES &&
      err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
    return h2_esp_wifi_map_error(err);
  }
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    return H2_PAL_ERR_INVALID_STATE;
  }

  int rc = h2_esp_platform_netif_init_once();
  if (rc != H2_PAL_OK)
    return rc;

  if (s_h2_esp_wifi_events == NULL) {
    s_h2_esp_wifi_events = xEventGroupCreate();
    if (s_h2_esp_wifi_events == NULL) {
      return H2_PAL_ERR_NO_MEMORY;
    }
  }

  /* Create the default STA netif before registering the observer below so
   * ESP-NETIF's route-owning handlers commit their state first. */
  if (s_h2_esp_wifi_sta_netif == NULL) {
    s_h2_esp_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_h2_esp_wifi_sta_netif == NULL) {
      return H2_PAL_ERR_NO_MEMORY;
    }
  }

  if (s_h2_esp_wifi_events_registered == 0) {
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     h2_esp_wifi_event_handler, NULL);
    if (err != ESP_OK) {
      return h2_esp_wifi_map_error(err);
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     h2_esp_wifi_event_handler, NULL);
    if (err != ESP_OK) {
      return h2_esp_wifi_map_error(err);
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                     h2_esp_wifi_event_handler, NULL);
    if (err != ESP_OK) {
      return h2_esp_wifi_map_error(err);
    }
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                     h2_esp_wifi_event_handler, NULL);
    if (err != ESP_OK) {
      return h2_esp_wifi_map_error(err);
    }
#endif
    s_h2_esp_wifi_events_registered = 1;
  }

  wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
  safe_call.op = H2_ESP_WIFI_SAFE_INIT;
  safe_call.init_config = init_config;
  err = h2_esp_wifi_run_safe(&safe_call);
  if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
    return h2_esp_wifi_map_error(err);
  }

  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) {
    return h2_esp_wifi_map_error(err);
  }

  err = esp_wifi_start();
  if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
    return h2_esp_wifi_map_error(err);
  }

  s_h2_esp_wifi_started = 1;
  return H2_PAL_OK;
}

static h2_pal_wifi_security_t h2_esp_wifi_security(wifi_auth_mode_t auth) {
  switch (auth) {
  case WIFI_AUTH_OPEN:
    return H2_PAL_WIFI_SECURITY_OPEN;
  case WIFI_AUTH_WEP:
    return H2_PAL_WIFI_SECURITY_WEP;
  case WIFI_AUTH_WPA_PSK:
    return H2_PAL_WIFI_SECURITY_WPA;
  case WIFI_AUTH_WPA2_PSK:
    return H2_PAL_WIFI_SECURITY_WPA2;
  case WIFI_AUTH_WPA_WPA2_PSK:
    return H2_PAL_WIFI_SECURITY_WPA_WPA2;
  case WIFI_AUTH_WPA3_PSK:
    return H2_PAL_WIFI_SECURITY_WPA3;
  case WIFI_AUTH_WPA2_WPA3_PSK:
    return H2_PAL_WIFI_SECURITY_WPA2_WPA3;
  case WIFI_AUTH_ENTERPRISE:
  case WIFI_AUTH_WPA3_ENTERPRISE:
  case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
  case WIFI_AUTH_WPA_ENTERPRISE:
    return H2_PAL_WIFI_SECURITY_ENTERPRISE;
  default:
    return H2_PAL_WIFI_SECURITY_UNKNOWN;
  }
}

static void h2_esp_wifi_copy_ap_record(h2_pal_wifi_scan_entry_t *out_entry,
                                       const wifi_ap_record_t *record) {
  memset(out_entry, 0, sizeof(*out_entry));
  out_entry->ssid_len = h2_esp_wifi_strnlen(record->ssid, H2_PAL_WIFI_SSID_MAX);
  memcpy(out_entry->ssid, record->ssid, out_entry->ssid_len);
  out_entry->ssid[out_entry->ssid_len] = '\0';
  memcpy(out_entry->bssid, record->bssid, sizeof(out_entry->bssid));
  out_entry->channel = record->primary;
  out_entry->rssi = record->rssi;
  out_entry->security = h2_esp_wifi_security(record->authmode);
}

static void h2_esp_wifi_status_from_ap(h2_pal_wifi_sta_status_t *out_status,
                                       const wifi_ap_record_t *record) {
  memset(out_status, 0, sizeof(*out_status));
  out_status->state = H2_PAL_WIFI_STA_STATE_CONNECTED;
  out_status->ssid_len =
      h2_esp_wifi_strnlen(record->ssid, H2_PAL_WIFI_SSID_MAX);
  memcpy(out_status->ssid, record->ssid, out_status->ssid_len);
  out_status->ssid[out_status->ssid_len] = '\0';
  memcpy(out_status->bssid, record->bssid, sizeof(out_status->bssid));
  out_status->bssid_set = 1u;
  out_status->channel = record->primary;
  out_status->rssi = record->rssi;
}

static void
h2_esp_wifi_copy_sta_config(wifi_config_t *out_config,
                            const h2_pal_wifi_sta_config_t *config) {
  memset(out_config, 0, sizeof(*out_config));
  memcpy(out_config->sta.ssid, config->ssid, config->ssid_len);
  memcpy(out_config->sta.password, config->password, config->password_len);
  out_config->sta.bssid_set = config->bssid_set != 0u;
  memcpy(out_config->sta.bssid, config->bssid, sizeof(out_config->sta.bssid));
  out_config->sta.channel = config->channel;
}

#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
static wifi_auth_mode_t h2_esp_wifi_auth_mode(h2_pal_wifi_security_t security) {
  switch (security) {
  case H2_PAL_WIFI_SECURITY_OPEN:
    return WIFI_AUTH_OPEN;
  case H2_PAL_WIFI_SECURITY_WPA:
    return WIFI_AUTH_WPA_PSK;
  case H2_PAL_WIFI_SECURITY_WPA2:
    return WIFI_AUTH_WPA2_PSK;
  case H2_PAL_WIFI_SECURITY_WPA3:
    return WIFI_AUTH_WPA3_PSK;
  case H2_PAL_WIFI_SECURITY_WPA_WPA2:
    return WIFI_AUTH_WPA_WPA2_PSK;
  case H2_PAL_WIFI_SECURITY_WPA2_WPA3:
    return WIFI_AUTH_WPA2_WPA3_PSK;
  default:
    return WIFI_AUTH_WPA2_PSK;
  }
}

static void h2_esp_wifi_copy_ap_config(wifi_config_t *out_config,
                                       const h2_pal_wifi_ap_config_t *config) {
  memset(out_config, 0, sizeof(*out_config));
  memcpy(out_config->ap.ssid, config->ssid, config->ssid_len);
  out_config->ap.ssid_len = (uint8_t)config->ssid_len;
  memcpy(out_config->ap.password, config->password, config->password_len);
  out_config->ap.channel = config->channel != 0u ? config->channel : 1u;
  out_config->ap.max_connection = config->max_clients != 0u
                                      ? config->max_clients
                                      : H2_PAL_WIFI_AP_MAX_CLIENTS;
  out_config->ap.authmode = h2_esp_wifi_auth_mode(config->security);
  out_config->ap.ssid_hidden = config->hidden != 0u ? 1u : 0u;
  if (config->security == H2_PAL_WIFI_SECURITY_OPEN) {
    out_config->ap.authmode = WIFI_AUTH_OPEN;
  }
}

static void
h2_esp_wifi_set_ap_status_from_config(const h2_pal_wifi_ap_config_t *config) {
  memset(&s_h2_esp_wifi_ap_status, 0, sizeof(s_h2_esp_wifi_ap_status));
  s_h2_esp_wifi_ap_status.state = H2_PAL_WIFI_AP_STATE_STARTED;
  s_h2_esp_wifi_ap_status.ssid_len = config->ssid_len;
  memcpy(s_h2_esp_wifi_ap_status.ssid, config->ssid, config->ssid_len);
  s_h2_esp_wifi_ap_status.ssid[s_h2_esp_wifi_ap_status.ssid_len] = '\0';
  s_h2_esp_wifi_ap_status.channel =
      config->channel != 0u ? config->channel : 1u;
  s_h2_esp_wifi_ap_status.max_clients = config->max_clients != 0u
                                            ? config->max_clients
                                            : H2_PAL_WIFI_AP_MAX_CLIENTS;
  s_h2_esp_wifi_ap_status.security = config->security;
  s_h2_esp_wifi_ap_status.hidden = config->hidden;
  s_h2_esp_wifi_ap_status.client_count = s_h2_esp_wifi_ap_client_count;
}

static int h2_esp_wifi_ap_config_same(const h2_pal_wifi_ap_config_t *config) {
  portENTER_CRITICAL(&s_h2_esp_wifi_ap_lock);
  int same = s_h2_esp_wifi_ap_active != 0 &&
             s_h2_esp_wifi_ap_config.ssid_len == config->ssid_len &&
             s_h2_esp_wifi_ap_config.password_len == config->password_len &&
             s_h2_esp_wifi_ap_config.channel == config->channel &&
             s_h2_esp_wifi_ap_config.max_clients == config->max_clients &&
             s_h2_esp_wifi_ap_config.security == config->security &&
             s_h2_esp_wifi_ap_config.hidden == config->hidden &&
             memcmp(s_h2_esp_wifi_ap_config.ssid, config->ssid,
                    config->ssid_len) == 0 &&
             memcmp(s_h2_esp_wifi_ap_config.password, config->password,
                    config->password_len) == 0;
  portEXIT_CRITICAL(&s_h2_esp_wifi_ap_lock);
  return same;
}
#endif

static int h2_esp_wifi_sta_get_status(h2_pal_wifi_sta_t *sta,
                                      h2_pal_wifi_sta_status_t *out_status) {
  (void)sta;
  if (out_status == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_status, 0, sizeof(*out_status));

  int rc = h2_esp_platform_wifi_ensure_started();
  if (rc != H2_PAL_OK) {
    return rc;
  }

  wifi_ap_record_t ap_info;
  memset(&ap_info, 0, sizeof(ap_info));
  esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
  if (err == ESP_OK) {
    h2_esp_wifi_status_from_ap(out_status, &ap_info);
    if (s_h2_esp_wifi_events != NULL) {
      EventBits_t bits = xEventGroupGetBits(s_h2_esp_wifi_events);
      if ((bits & H2_ESP_WIFI_EVENT_GOT_IP) != 0u) {
        out_status->state = H2_PAL_WIFI_STA_STATE_GOT_IP;
        esp_netif_ip_info_t ip_info;
        memset(&ip_info, 0, sizeof(ip_info));
        if (s_h2_esp_wifi_sta_netif != NULL &&
            esp_netif_get_ip_info(s_h2_esp_wifi_sta_netif, &ip_info) ==
                ESP_OK) {
          out_status->ip.ip4 = lwip_ntohl(ip_info.ip.addr);
          out_status->ip.netmask4 = lwip_ntohl(ip_info.netmask.addr);
          out_status->ip.gateway4 = lwip_ntohl(ip_info.gw.addr);
          out_status->ip_valid = 1u;
        }
      }
    }
    return H2_PAL_OK;
  }
  if (err != ESP_ERR_WIFI_NOT_CONNECT && err != ESP_ERR_WIFI_NOT_ASSOC) {
    return h2_esp_wifi_map_error(err);
  }

  out_status->state = H2_PAL_WIFI_STA_STATE_IDLE;
  out_status->disconnect_reason = s_h2_esp_wifi_sta_disconnect_reason;
  wifi_config_t config;
  memset(&config, 0, sizeof(config));
  err = esp_wifi_get_config(WIFI_IF_STA, &config);
  if (err == ESP_OK) {
    out_status->ssid_len =
        h2_esp_wifi_strnlen(config.sta.ssid, H2_PAL_WIFI_SSID_MAX);
    memcpy(out_status->ssid, config.sta.ssid, out_status->ssid_len);
    out_status->ssid[out_status->ssid_len] = '\0';
    if (out_status->ssid_len > 0u) {
      out_status->state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
    }
  }
  return H2_PAL_OK;
}

static int h2_esp_wifi_sta_scan(h2_pal_wifi_sta_t *sta,
                                const h2_pal_wifi_scan_request_t *request,
                                h2_pal_wifi_scan_result_fn on_result,
                                void *user, uint32_t timeout_ms) {
  (void)sta;
  (void)timeout_ms;
  if (on_result == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }

  int rc = h2_esp_platform_wifi_ensure_started();
  if (rc != H2_PAL_OK) {
    return rc;
  }

  wifi_scan_config_t scan_config;
  memset(&scan_config, 0, sizeof(scan_config));
  uint8_t ssid[H2_PAL_WIFI_SSID_MAX + 1u];
  if (request != NULL && request->ssid_len > 0u) {
    memset(ssid, 0, sizeof(ssid));
    memcpy(ssid, request->ssid, request->ssid_len);
    scan_config.ssid = ssid;
  }
  if (request != NULL && request->channel != 0u) {
    scan_config.channel = request->channel;
  }

  h2_esp_wifi_set_activity(1);
  esp_err_t err = esp_wifi_scan_start(&scan_config, true);
  h2_esp_wifi_set_activity(0);
  if (err != ESP_OK) {
    return h2_esp_wifi_map_error(err);
  }

  wifi_ap_record_t records[H2_PAL_WIFI_SCAN_MAX_RESULTS];
  memset(records, 0, sizeof(records));
  uint16_t count = H2_PAL_WIFI_SCAN_MAX_RESULTS;
  err = esp_wifi_scan_get_ap_records(&count, records);
  if (err != ESP_OK) {
    return h2_esp_wifi_map_error(err);
  }

  for (uint16_t i = 0u; i < count; ++i) {
    h2_pal_wifi_scan_entry_t entry;
    h2_esp_wifi_copy_ap_record(&entry, &records[i]);
    if (!on_result(user, &entry)) {
      break;
    }
  }
  return H2_PAL_OK;
}

static int h2_esp_wifi_sta_connect(h2_pal_wifi_sta_t *sta,
                                   const h2_pal_wifi_sta_config_t *config,
                                   uint32_t timeout_ms) {
  (void)sta;
  int rc = h2_pal_wifi_settings_validate_sta_config(config);
  if (rc != H2_PAL_OK) {
    return rc;
  }

  rc = h2_esp_platform_wifi_ensure_started();
  if (rc != H2_PAL_OK) {
    return rc;
  }

  h2_esp_wifi_sta_reconnect_disable();
  h2_esp_wifi_set_activity(1);

  wifi_ap_record_t current_ap;
  memset(&current_ap, 0, sizeof(current_ap));
  int was_connected = esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK;

  wifi_config_t esp_config;
  h2_esp_wifi_copy_sta_config(&esp_config, config);
  esp_err_t err;
  rc = h2_esp_platform_wifi_set_config_safe(WIFI_IF_STA, &esp_config);
  if (rc != H2_PAL_OK) {
    h2_esp_wifi_set_activity(0);
    return rc;
  }

  if (s_h2_esp_wifi_events == NULL) {
    h2_esp_wifi_set_activity(0);
    return H2_PAL_ERR_UNAVAILABLE;
  }

  TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
  TickType_t started_at = xTaskGetTickCount();
  if (was_connected != 0) {
    xEventGroupClearBits(s_h2_esp_wifi_events,
                         H2_ESP_WIFI_EVENT_CONNECTED |
                             H2_ESP_WIFI_EVENT_DISCONNECTED |
                             H2_ESP_WIFI_EVENT_GOT_IP);
    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT &&
        err != ESP_ERR_WIFI_NOT_ASSOC) {
      h2_esp_wifi_set_activity(0);
      return h2_esp_wifi_map_error(err);
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_h2_esp_wifi_events, H2_ESP_WIFI_EVENT_DISCONNECTED, pdTRUE, pdFALSE,
        timeout_ms == 0u ? portMAX_DELAY : timeout_ticks);
    if ((bits & H2_ESP_WIFI_EVENT_DISCONNECTED) == 0u) {
      h2_esp_wifi_set_activity(0);
      return H2_PAL_ERR_TIMEOUT;
    }
  }

  xEventGroupClearBits(s_h2_esp_wifi_events,
                       H2_ESP_WIFI_EVENT_CONNECTED |
                           H2_ESP_WIFI_EVENT_DISCONNECTED |
                           H2_ESP_WIFI_EVENT_GOT_IP);
  h2_pal_wifi_sta_status_t connecting_status;
  memset(&connecting_status, 0, sizeof(connecting_status));
  connecting_status.state = H2_PAL_WIFI_STA_STATE_CONNECTING;
  connecting_status.ssid_len = config->ssid_len;
  memcpy(connecting_status.ssid, config->ssid, config->ssid_len);
  connecting_status.ssid[connecting_status.ssid_len] = '\0';
  h2_esp_wifi_post_sta_system_event(
      H2_PAL_SYSTEM_EVENT_TYPE_WIFI_STA_CONNECTING, &connecting_status);
  h2_esp_wifi_sta_reconnect_enable();
  err = esp_wifi_connect();
  if (err != ESP_OK) {
    h2_esp_wifi_sta_reconnect_disable();
    h2_esp_wifi_set_activity(0);
    return h2_esp_wifi_map_error(err);
  }
  if (timeout_ms == 0u) {
    h2_esp_wifi_set_activity(0);
    return H2_PAL_OK;
  }

  for (;;) {
    TickType_t elapsed = xTaskGetTickCount() - started_at;
    TickType_t remaining =
        elapsed < timeout_ticks ? timeout_ticks - elapsed : 0u;
    EventBits_t bits = xEventGroupWaitBits(s_h2_esp_wifi_events,
                                           H2_ESP_WIFI_EVENT_CONNECTED |
                                               H2_ESP_WIFI_EVENT_DISCONNECTED,
                                           pdFALSE, pdFALSE, remaining);
    if ((bits & H2_ESP_WIFI_EVENT_CONNECTED) != 0u) {
      wifi_ap_record_t ap_info;
      memset(&ap_info, 0, sizeof(ap_info));
      if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        size_t ssid_len =
            h2_esp_wifi_strnlen(ap_info.ssid, H2_PAL_WIFI_SSID_MAX);
        if (ssid_len == config->ssid_len &&
            memcmp(ap_info.ssid, config->ssid, ssid_len) == 0) {
          h2_esp_wifi_set_activity(0);
          return H2_PAL_OK;
        }
      }
    }
    if ((bits & H2_ESP_WIFI_EVENT_DISCONNECTED) == 0u ||
        !h2_esp_wifi_sta_reconnect_is_enabled() || remaining == 0u) {
      break;
    }
    xEventGroupClearBits(s_h2_esp_wifi_events, H2_ESP_WIFI_EVENT_DISCONNECTED);
  }

  h2_esp_wifi_set_activity(0);
  return H2_PAL_ERR_TIMEOUT;
}

static int h2_esp_wifi_sta_disconnect(h2_pal_wifi_sta_t *sta) {
  (void)sta;
  h2_esp_wifi_sta_reconnect_disable();
  if (s_h2_esp_wifi_started == 0) {
    if (s_h2_esp_wifi_events != NULL) {
      xEventGroupClearBits(s_h2_esp_wifi_events, H2_ESP_WIFI_EVENT_CONNECTED |
                                                     H2_ESP_WIFI_EVENT_GOT_IP);
      xEventGroupSetBits(s_h2_esp_wifi_events, H2_ESP_WIFI_EVENT_DISCONNECTED);
    }
    return H2_PAL_OK;
  }
  h2_esp_wifi_set_activity(1);
  esp_err_t err = esp_wifi_disconnect();
  h2_esp_wifi_set_activity(0);
  if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT &&
      err != ESP_ERR_WIFI_NOT_ASSOC) {
    return h2_esp_wifi_map_error(err);
  }

#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
  if (s_h2_esp_wifi_ap_active != 0) {
    return h2_esp_wifi_map_error(esp_wifi_set_mode(WIFI_MODE_AP));
  }
#endif
  if (s_h2_esp_wifi_events != NULL) {
    xEventGroupClearBits(s_h2_esp_wifi_events, H2_ESP_WIFI_EVENT_CONNECTED |
                                                   H2_ESP_WIFI_EVENT_GOT_IP);
    xEventGroupSetBits(s_h2_esp_wifi_events, H2_ESP_WIFI_EVENT_DISCONNECTED);
  }
  return H2_PAL_OK;
}

static int h2_esp_wifi_sta_set_power_save(
    h2_pal_wifi_sta_t *sta,
    h2_pal_wifi_power_save_t mode) {
    (void)sta;
    wifi_ps_type_t type;
    switch (mode) {
    case H2_PAL_WIFI_POWER_SAVE_NONE:
        type = WIFI_PS_NONE;
        break;
    case H2_PAL_WIFI_POWER_SAVE_MIN_MODEM:
        type = WIFI_PS_MIN_MODEM;
        break;
    case H2_PAL_WIFI_POWER_SAVE_MAX_MODEM:
        type = WIFI_PS_MAX_MODEM;
        break;
    default:
        return H2_PAL_ERR_INVALID_ARG;
    }
    int rc = h2_esp_platform_wifi_ensure_started();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return h2_esp_wifi_map_error(esp_wifi_set_ps(type));
}

static int h2_esp_wifi_sta_get_mac(h2_pal_wifi_sta_t *sta, uint8_t out_mac[6]) {
  (void)sta;
  if (out_mac == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  int rc = h2_esp_platform_wifi_ensure_started();
  if (rc != H2_PAL_OK) {
    return rc;
  }
  return h2_esp_wifi_map_error(esp_wifi_get_mac(WIFI_IF_STA, out_mac));
}

#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
static int h2_esp_wifi_ap_start(h2_pal_wifi_ap_t *ap,
                                const h2_pal_wifi_ap_config_t *config,
                                uint32_t timeout_ms) {
  (void)ap;
  (void)timeout_ms;

  int rc = h2_pal_wifi_ap_config_validate(config);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (h2_esp_wifi_ap_config_same(config)) {
    return H2_PAL_OK;
  }

  rc = h2_esp_platform_wifi_ensure_started();
  if (rc != H2_PAL_OK) {
    return rc;
  }
  if (s_h2_esp_wifi_ap_netif == NULL) {
    s_h2_esp_wifi_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_h2_esp_wifi_ap_netif == NULL) {
      return H2_PAL_ERR_NO_MEMORY;
    }
  }

  wifi_config_t wifi_config;
  h2_esp_wifi_copy_ap_config(&wifi_config, config);
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err != ESP_OK) {
    if (s_h2_esp_wifi_ap_netif != NULL) {
      esp_netif_destroy_default_wifi(s_h2_esp_wifi_ap_netif);
      s_h2_esp_wifi_ap_netif = NULL;
    }
    return h2_esp_wifi_map_error(err);
  }
  rc = h2_esp_platform_wifi_set_config_safe(WIFI_IF_AP, &wifi_config);
  if (rc != H2_PAL_OK) {
    if (s_h2_esp_wifi_ap_netif != NULL) {
      esp_netif_destroy_default_wifi(s_h2_esp_wifi_ap_netif);
      s_h2_esp_wifi_ap_netif = NULL;
    }
    return rc;
  }
  err = esp_wifi_start();
  if (err != ESP_OK && err != ESP_ERR_WIFI_CONN && err != ESP_ERR_WIFI_STATE) {
    if (s_h2_esp_wifi_ap_netif != NULL) {
      esp_netif_destroy_default_wifi(s_h2_esp_wifi_ap_netif);
      s_h2_esp_wifi_ap_netif = NULL;
    }
    return h2_esp_wifi_map_error(err);
  }

  h2_pal_wifi_ap_status_t status;
  portENTER_CRITICAL(&s_h2_esp_wifi_ap_lock);
  s_h2_esp_wifi_ap_active = 1;
  s_h2_esp_wifi_ap_config = *config;
  memset(s_h2_esp_wifi_ap_clients, 0, sizeof(s_h2_esp_wifi_ap_clients));
  s_h2_esp_wifi_ap_client_count = 0u;
  h2_esp_wifi_set_ap_status_from_config(config);
  status = s_h2_esp_wifi_ap_status;
  portEXIT_CRITICAL(&s_h2_esp_wifi_ap_lock);
  h2_esp_wifi_post_ap_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STARTED,
                                   &status);
  return H2_PAL_OK;
}

static int h2_esp_wifi_ap_stop(h2_pal_wifi_ap_t *ap, uint32_t timeout_ms) {
  (void)ap;
  (void)timeout_ms;
  portENTER_CRITICAL(&s_h2_esp_wifi_ap_lock);
  int was_active = s_h2_esp_wifi_ap_active;
  portEXIT_CRITICAL(&s_h2_esp_wifi_ap_lock);
  if (was_active == 0) {
    return H2_PAL_OK;
  }

  h2_pal_wifi_ap_status_t status;
  portENTER_CRITICAL(&s_h2_esp_wifi_ap_lock);
  s_h2_esp_wifi_ap_active = 0;
  s_h2_esp_wifi_ap_client_count = 0u;
  memset(s_h2_esp_wifi_ap_clients, 0, sizeof(s_h2_esp_wifi_ap_clients));
  s_h2_esp_wifi_ap_status.state = H2_PAL_WIFI_AP_STATE_STOPPED;
  s_h2_esp_wifi_ap_status.client_count = 0u;
  status = s_h2_esp_wifi_ap_status;
  portEXIT_CRITICAL(&s_h2_esp_wifi_ap_lock);
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT &&
      err != ESP_ERR_WIFI_NOT_STARTED) {
    return h2_esp_wifi_map_error(err);
  }
  if (s_h2_esp_wifi_ap_netif != NULL) {
    esp_netif_destroy_default_wifi(s_h2_esp_wifi_ap_netif);
    s_h2_esp_wifi_ap_netif = NULL;
  }
  h2_esp_wifi_post_ap_system_event(H2_PAL_SYSTEM_EVENT_TYPE_WIFI_AP_STOPPED,
                                   &status);
  return h2_esp_wifi_stop_driver_if_sta_idle();
}

static int h2_esp_wifi_ap_get_status(h2_pal_wifi_ap_t *ap,
                                     h2_pal_wifi_ap_status_t *out_status) {
  (void)ap;
  if (out_status == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_status, 0, sizeof(*out_status));
  portENTER_CRITICAL(&s_h2_esp_wifi_ap_lock);
  if (s_h2_esp_wifi_ap_active == 0) {
    out_status->state = H2_PAL_WIFI_AP_STATE_STOPPED;
    portEXIT_CRITICAL(&s_h2_esp_wifi_ap_lock);
    return H2_PAL_OK;
  }
  s_h2_esp_wifi_ap_status.client_count = s_h2_esp_wifi_ap_client_count;
  *out_status = s_h2_esp_wifi_ap_status;
  portEXIT_CRITICAL(&s_h2_esp_wifi_ap_lock);
  return H2_PAL_OK;
}

static int h2_esp_wifi_ap_get_clients(h2_pal_wifi_ap_t *ap,
                                      h2_pal_wifi_ap_client_t *out_clients,
                                      size_t max_clients, size_t *out_count) {
  (void)ap;
  if (out_count == NULL || (max_clients > 0u && out_clients == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  portENTER_CRITICAL(&s_h2_esp_wifi_ap_lock);
  if (s_h2_esp_wifi_ap_active == 0) {
    *out_count = 0u;
    portEXIT_CRITICAL(&s_h2_esp_wifi_ap_lock);
    return H2_PAL_OK;
  }
  size_t copy_count = s_h2_esp_wifi_ap_client_count;
  if (copy_count > max_clients) {
    copy_count = max_clients;
  }
  if (copy_count > 0u) {
    memcpy(out_clients, s_h2_esp_wifi_ap_clients,
           copy_count * sizeof(out_clients[0]));
  }
  *out_count = copy_count;
  portEXIT_CRITICAL(&s_h2_esp_wifi_ap_lock);
  return H2_PAL_OK;
}

static int h2_esp_wifi_ap_get_mac(h2_pal_wifi_ap_t *ap, uint8_t out_mac[6]) {
  (void)ap;
  if (out_mac == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  int rc = h2_esp_platform_wifi_ensure_started();
  if (rc != H2_PAL_OK) {
    return rc;
  }
  return h2_esp_wifi_map_error(esp_wifi_get_mac(WIFI_IF_AP, out_mac));
}
#else
static int h2_esp_wifi_ap_start(h2_pal_wifi_ap_t *ap,
                                const h2_pal_wifi_ap_config_t *config,
                                uint32_t timeout_ms) {
  (void)ap;
  (void)config;
  (void)timeout_ms;
  return H2_PAL_ERR_UNSUPPORTED;
}

static int h2_esp_wifi_ap_stop(h2_pal_wifi_ap_t *ap, uint32_t timeout_ms) {
  (void)ap;
  (void)timeout_ms;
  return H2_PAL_OK;
}

static int h2_esp_wifi_ap_get_status(h2_pal_wifi_ap_t *ap,
                                     h2_pal_wifi_ap_status_t *out_status) {
  (void)ap;
  if (out_status == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_status, 0, sizeof(*out_status));
  out_status->state = H2_PAL_WIFI_AP_STATE_STOPPED;
  return H2_PAL_OK;
}

static int h2_esp_wifi_ap_get_clients(h2_pal_wifi_ap_t *ap,
                                      h2_pal_wifi_ap_client_t *out_clients,
                                      size_t max_clients, size_t *out_count) {
  (void)ap;
  (void)out_clients;
  (void)max_clients;
  if (out_count == NULL || (max_clients > 0u && out_clients == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_count = 0u;
  return H2_PAL_ERR_UNSUPPORTED;
}

static int h2_esp_wifi_ap_get_mac(h2_pal_wifi_ap_t *ap, uint8_t out_mac[6]) {
  (void)ap;
  (void)out_mac;
  return H2_PAL_ERR_UNSUPPORTED;
}
#endif

static const h2_pal_wifi_sta_vtable_t s_h2_esp_wifi_sta_vtable = {
    .get_status = (h2_pal_wifi_sta_get_status_fn)h2_esp_wifi_sta_get_status,
    .scan = (h2_pal_wifi_sta_scan_fn)h2_esp_wifi_sta_scan,
    .connect = (h2_pal_wifi_sta_connect_fn)h2_esp_wifi_sta_connect,
    .disconnect = (h2_pal_wifi_sta_disconnect_fn)h2_esp_wifi_sta_disconnect,
    .get_mac = (h2_pal_wifi_sta_get_mac_fn)h2_esp_wifi_sta_get_mac,
    .set_power_save =
        (h2_pal_wifi_sta_set_power_save_fn)h2_esp_wifi_sta_set_power_save,
};

static h2_pal_wifi_sta_t s_h2_esp_wifi_sta = {
    .user = &s_h2_esp_wifi_sta,
    .vtable = &s_h2_esp_wifi_sta_vtable,
};

static const h2_pal_wifi_ap_vtable_t s_h2_esp_wifi_ap_vtable = {
    .start = (h2_pal_wifi_ap_start_fn)h2_esp_wifi_ap_start,
    .stop = (h2_pal_wifi_ap_stop_fn)h2_esp_wifi_ap_stop,
    .get_status = (h2_pal_wifi_ap_get_status_fn)h2_esp_wifi_ap_get_status,
    .get_clients = (h2_pal_wifi_ap_get_clients_fn)h2_esp_wifi_ap_get_clients,
    .get_mac = (h2_pal_wifi_ap_get_mac_fn)h2_esp_wifi_ap_get_mac,
};

static h2_pal_wifi_ap_t s_h2_esp_wifi_ap = {
    .user = &s_h2_esp_wifi_ap,
    .vtable = &s_h2_esp_wifi_ap_vtable,
};

h2_pal_wifi_sta_t *h2_esp_platform_wifi_sta(void) { return &s_h2_esp_wifi_sta; }

h2_pal_wifi_ap_t *h2_esp_platform_wifi_ap(void) { return &s_h2_esp_wifi_ap; }

int h2_esp_platform_wifi_connect_saved(uint32_t timeout_ms) {
  int rc = h2_esp_platform_wifi_ensure_started();
  if (rc != H2_PAL_OK) {
    return rc;
  }

  wifi_config_t esp_config;
  memset(&esp_config, 0, sizeof(esp_config));
  esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &esp_config);
  if (err != ESP_OK) {
    return h2_esp_wifi_map_error(err);
  }
  size_t ssid_len =
      h2_esp_wifi_strnlen(esp_config.sta.ssid, H2_PAL_WIFI_SSID_MAX);
  if (ssid_len == 0u) {
    return H2_PAL_ERR_NOT_FOUND;
  }

  h2_pal_wifi_sta_config_t config;
  memset(&config, 0, sizeof(config));
  config.ssid_len = ssid_len;
  memcpy(config.ssid, esp_config.sta.ssid, ssid_len);
  config.ssid[ssid_len] = '\0';
  config.password_len =
      h2_esp_wifi_strnlen(esp_config.sta.password, H2_PAL_WIFI_PASSWORD_MAX);
  memcpy(config.password, esp_config.sta.password, config.password_len);
  config.password[config.password_len] = '\0';
  config.bssid_set = esp_config.sta.bssid_set ? 1u : 0u;
  memcpy(config.bssid, esp_config.sta.bssid, sizeof(config.bssid));
  config.channel = esp_config.sta.channel;

  TickType_t timeout_ticks =
      timeout_ms == 0u ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  TickType_t started_at = xTaskGetTickCount();
  rc = h2_esp_wifi_sta_connect(&s_h2_esp_wifi_sta, &config, timeout_ms);
  if (rc != H2_PAL_OK) {
    return rc;
  }

  EventBits_t current = xEventGroupGetBits(s_h2_esp_wifi_events);
  if ((current & H2_ESP_WIFI_EVENT_GOT_IP) != 0u) {
    return H2_PAL_OK;
  }

  TickType_t elapsed = xTaskGetTickCount() - started_at;
  TickType_t remaining = timeout_ms == 0u || elapsed < timeout_ticks
                             ? timeout_ticks - elapsed
                             : 0u;
  EventBits_t bits = xEventGroupWaitBits(s_h2_esp_wifi_events,
                                         H2_ESP_WIFI_EVENT_GOT_IP |
                                             H2_ESP_WIFI_EVENT_DISCONNECTED,
                                         pdFALSE, pdFALSE, remaining);
  if ((bits & H2_ESP_WIFI_EVENT_GOT_IP) != 0u) {
    return H2_PAL_OK;
  }
  if ((bits & H2_ESP_WIFI_EVENT_DISCONNECTED) != 0u) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  return H2_PAL_ERR_TIMEOUT;
}
