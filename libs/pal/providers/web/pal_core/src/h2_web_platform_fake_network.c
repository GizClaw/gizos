#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

/* Only radio state is simulated. HTTP/WebRTC retain the browser transports.
 * All JS reads run on the main thread; mutable PAL state is per-platform and
 * serialized by this mutex. Never hold it across a main-thread call. */
typedef struct h2_web_fake_network {
  pthread_mutex_t mutex;
  h2_pal_wifi_sta_api_t wifi;
  h2_pal_wifi_settings_api_t settings;
  h2_pal_modem_api_t modem;
  h2_pal_wifi_sta_config_t saved;
  bool saved_valid, wifi_connected, modem_open, data_open;
  h2_pal_modem_power_policy_t policy;
} h2_web_fake_network_t;

typedef struct environment {
  int32_t flags[8];
  char ssid[H2_PAL_WIFI_SSID_MAX + 1];
} environment_t;
enum {
  H2_WEB_FAKE_WIFI_ENABLED,
  H2_WEB_FAKE_WIFI_CONNECTED,
  H2_WEB_FAKE_WIFI_RSSI,
  H2_WEB_FAKE_MODEM_ENABLED,
  H2_WEB_FAKE_SIM_PRESENT,
  H2_WEB_FAKE_MODEM_REGISTERED,
  H2_WEB_FAKE_MODEM_RSSI,
  H2_WEB_FAKE_HOST_ONLINE
};

/* clang-format off */
EM_JS(void, fake_environment_js,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["pointer", "pointer"], "i32", (flags, ssid) => {
    const env = Module['h2WebEnvironment'];
    if (env === undefined) return 0;
    if (!env || env.version !== 1 || !env.wifi || !env.modem) return -1;
    const w = env.wifi, m = env.modem;
    if ([w.enabled, w.connected, m.enabled, m.simPresent, m.registered].some(v => typeof v !== 'boolean') ||
        typeof w.ssid !== 'string' || !w.ssid || w.ssid.includes('\0') || lengthBytesUTF8(w.ssid) > 32 ||
        !Number.isInteger(w.rssi) || w.rssi < -127 || w.rssi > 0 ||
        !Number.isInteger(m.rssi) || m.rssi < -127 || m.rssi > 0) return -1;
    const values = [w.enabled, w.connected, w.rssi, m.enabled, m.simPresent,
      m.registered, m.rssi, globalThis.navigator?.onLine !== false];
    values.forEach((v, i) => { HEAP32[(flags >> 2) + i] = Number(v); });
    stringToUTF8(w.ssid, ssid, 33);
    return 1;
  });
});
/* clang-format on */

static int environment_read(environment_t *env) {
  memset(env, 0, sizeof(*env));
  int32_t *flags = env->flags;
  char *ssid = env->ssid;
  return (int)h2_web_main_call(fake_environment_js,
                               (const void *[]){&flags, &ssid})
      .i32;
}

static h2_pal_result_t snapshot(h2_web_platform_t *p, environment_t *env) {
  if (p == NULL || p->fake_network == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  return environment_read(env) == 1 ? H2_PAL_OK : H2_PAL_ERR_INVALID_ARG;
}

static bool wifi_online(const h2_web_fake_network_t *s,
                        const environment_t *e) {
  return e->flags[H2_WEB_FAKE_HOST_ONLINE] &&
         e->flags[H2_WEB_FAKE_WIFI_ENABLED] &&
         e->flags[H2_WEB_FAKE_WIFI_CONNECTED] && s->wifi_connected;
}
static bool modem_registered(const h2_web_fake_network_t *s,
                             const environment_t *e) {
  return e->flags[H2_WEB_FAKE_HOST_ONLINE] &&
         e->flags[H2_WEB_FAKE_MODEM_ENABLED] &&
         e->flags[H2_WEB_FAKE_SIM_PRESENT] &&
         e->flags[H2_WEB_FAKE_MODEM_REGISTERED] && s->modem_open;
}
static void changed(h2_web_platform_t *p) {
  atomic_store(&p->netif_dirty, true);
  h2_web_platform_schedule(p);
}

bool h2_web_platform_fake_network_available(h2_web_platform_t *p) {
  if (p->fake_network == NULL)
    return true;
  environment_t e;
  if (snapshot(p, &e) != H2_PAL_OK)
    return false;
  h2_web_fake_network_t *s = p->fake_network;
  pthread_mutex_lock(&s->mutex);
  const bool available =
      wifi_online(s, &e) || (modem_registered(s, &e) && s->data_open);
  pthread_mutex_unlock(&s->mutex);
  return available;
}

static int wifi_status(void *user, h2_pal_wifi_sta_status_t *out) {
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  h2_web_platform_t *p = user;
  environment_t e;
  int rc = snapshot(p, &e);
  if (rc != H2_PAL_OK)
    return rc;
  h2_web_fake_network_t *s = p->fake_network;
  pthread_mutex_lock(&s->mutex);
  const bool connected = wifi_online(s, &e);
  pthread_mutex_unlock(&s->mutex);
  out->state = connected ? H2_PAL_WIFI_STA_STATE_GOT_IP
                         : H2_PAL_WIFI_STA_STATE_DISCONNECTED;
  if (connected) {
    memcpy(out->ssid, e.ssid, sizeof(out->ssid));
    out->ssid_len = strlen(e.ssid);
    out->rssi = e.flags[H2_WEB_FAKE_WIFI_RSSI];
    out->channel = 1;
    out->bssid[0] = 2;
    out->bssid[5] = 1;
    out->bssid_set = 1;
    /* Documentation-only address range, explicitly a simulated lease. */
    out->ip = (h2_pal_wifi_ip_info_t){0xc0000202u, 0xffffff00u, 0xc0000201u};
    out->ip_valid = 1;
  }
  return H2_PAL_OK;
}
static int wifi_scan(void *user, const h2_pal_wifi_scan_request_t *request,
                     h2_pal_wifi_scan_result_fn callback, void *callback_user,
                     uint32_t timeout_ms) {
  (void)timeout_ms;
  if (callback == NULL || (request && request->ssid_len > H2_PAL_WIFI_SSID_MAX))
    return H2_PAL_ERR_INVALID_ARG;
  environment_t e;
  int rc = snapshot(user, &e);
  if (rc != H2_PAL_OK)
    return rc;
  if (!e.flags[H2_WEB_FAKE_WIFI_ENABLED])
    return H2_PAL_ERR_UNAVAILABLE;
  const size_t len = strlen(e.ssid);
  if (request && ((request->channel && request->channel != 1) ||
                  (request->ssid_len && (request->ssid_len != len ||
                                         memcmp(request->ssid, e.ssid, len)))))
    return H2_PAL_OK;
  h2_pal_wifi_scan_entry_t entry = {.ssid_len = len,
                                    .channel = 1,
                                    .rssi = e.flags[H2_WEB_FAKE_WIFI_RSSI],
                                    .security = H2_PAL_WIFI_SECURITY_OPEN,
                                    .bssid = {2, 0, 0, 0, 0, 1}};
  memcpy(entry.ssid, e.ssid, sizeof(entry.ssid));
  (void)callback(callback_user, &entry);
  return H2_PAL_OK;
}
static int wifi_connect_impl(void *user, const h2_pal_wifi_sta_config_t *config,
                             bool save) {
  int rc = h2_pal_wifi_settings_validate_sta_config(config);
  if (rc != H2_PAL_OK)
    return rc;
  environment_t e;
  rc = snapshot(user, &e);
  if (rc != H2_PAL_OK)
    return rc;
  const uint8_t bssid[6] = {2, 0, 0, 0, 0, 1};
  if (!e.flags[H2_WEB_FAKE_HOST_ONLINE] || !e.flags[H2_WEB_FAKE_WIFI_ENABLED] ||
      !e.flags[H2_WEB_FAKE_WIFI_CONNECTED] ||
      config->ssid_len != strlen(e.ssid) ||
      memcmp(config->ssid, e.ssid, config->ssid_len) || config->password_len ||
      (config->channel && config->channel != 1) ||
      (config->bssid_set && memcmp(config->bssid, bssid, sizeof(bssid))))
    return H2_PAL_ERR_UNAVAILABLE;
  h2_web_platform_t *p = user;
  h2_web_fake_network_t *s = p->fake_network;
  pthread_mutex_lock(&s->mutex);
  s->wifi_connected = true;
  if (save) {
    s->saved = *config;
    s->saved.ssid[config->ssid_len] = '\0';
    s->saved_valid = true;
  }
  pthread_mutex_unlock(&s->mutex);
  changed(p);
  return H2_PAL_OK;
}
static int wifi_connect(void *u, const h2_pal_wifi_sta_config_t *c,
                        uint32_t timeout) {
  (void)timeout;
  return wifi_connect_impl(u, c, false);
}
static int wifi_connect_save(void *u, const h2_pal_wifi_sta_config_t *c,
                             uint32_t timeout) {
  (void)timeout;
  return wifi_connect_impl(u, c, true);
}
static int wifi_disconnect(void *user) {
  h2_web_platform_t *p = user;
  h2_web_fake_network_t *s = p->fake_network;
  pthread_mutex_lock(&s->mutex);
  s->wifi_connected = false;
  pthread_mutex_unlock(&s->mutex);
  changed(p);
  return H2_PAL_OK;
}
static int wifi_mac(void *user, uint8_t out[6]) {
  (void)user;
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  const uint8_t mac[6] = {2, 0, 0, 0, 0, 2};
  memcpy(out, mac, 6);
  return H2_PAL_OK;
}
static int wifi_power_save(void *user, h2_pal_wifi_power_save_t mode) {
  (void)user;
  return mode >= H2_PAL_WIFI_POWER_SAVE_NONE &&
                 mode <= H2_PAL_WIFI_POWER_SAVE_MAX_MODEM
             ? H2_PAL_OK
             : H2_PAL_ERR_INVALID_ARG;
}
static int saved_get(void *user, h2_pal_wifi_sta_config_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  h2_web_fake_network_t *s = ((h2_web_platform_t *)user)->fake_network;
  pthread_mutex_lock(&s->mutex);
  const bool valid = s->saved_valid;
  if (valid)
    *out = s->saved;
  pthread_mutex_unlock(&s->mutex);
  return valid ? H2_PAL_OK : H2_PAL_ERR_NOT_FOUND;
}
static int saved_set(void *user, const h2_pal_wifi_sta_config_t *config) {
  int rc = h2_pal_wifi_settings_validate_sta_config(config);
  if (rc != H2_PAL_OK)
    return rc;
  h2_web_fake_network_t *s = ((h2_web_platform_t *)user)->fake_network;
  pthread_mutex_lock(&s->mutex);
  s->saved = *config;
  s->saved.ssid[config->ssid_len] = '\0';
  s->saved.password[config->password_len] = '\0';
  s->saved_valid = true;
  pthread_mutex_unlock(&s->mutex);
  return H2_PAL_OK;
}
static int saved_clear(void *user) {
  h2_web_fake_network_t *s = ((h2_web_platform_t *)user)->fake_network;
  pthread_mutex_lock(&s->mutex);
  memset(&s->saved, 0, sizeof(s->saved));
  s->saved_valid = false;
  pthread_mutex_unlock(&s->mutex);
  return H2_PAL_OK;
}
static int saved_has(void *user, int *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  h2_web_fake_network_t *s = ((h2_web_platform_t *)user)->fake_network;
  pthread_mutex_lock(&s->mutex);
  *out = s->saved_valid;
  pthread_mutex_unlock(&s->mutex);
  return H2_PAL_OK;
}

static h2_pal_result_t modem_open(void *user, uint32_t timeout) {
  (void)timeout;
  environment_t e;
  int rc = snapshot(user, &e);
  if (rc != H2_PAL_OK)
    return rc;
  /* Opening the driver does not require coverage or an enabled radio. Keep
   * it usable when the host later changes an offline boot to cellular. */
  h2_web_platform_t *p = user;
  h2_web_fake_network_t *s = p->fake_network;
  pthread_mutex_lock(&s->mutex);
  s->modem_open = true;
  pthread_mutex_unlock(&s->mutex);
  changed(p);
  return H2_PAL_OK;
}
static h2_pal_result_t modem_close(void *user, uint32_t timeout) {
  (void)timeout;
  h2_web_platform_t *p = user;
  h2_web_fake_network_t *s = p->fake_network;
  pthread_mutex_lock(&s->mutex);
  s->modem_open = s->data_open = false;
  s->policy = H2_PAL_MODEM_POWER_POLICY_ACTIVE;
  pthread_mutex_unlock(&s->mutex);
  changed(p);
  return H2_PAL_OK;
}
static h2_pal_result_t modem_capabilities(void *user, uint32_t *out) {
  (void)user;
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  *out = H2_PAL_MODEM_CAPABILITY_DATA | H2_PAL_MODEM_CAPABILITY_LOW_POWER;
  return H2_PAL_OK;
}
static h2_pal_result_t modem_status(void *user, h2_pal_modem_status_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  environment_t e;
  int rc = snapshot(user, &e);
  if (rc != H2_PAL_OK)
    return rc;
  h2_web_fake_network_t *s = ((h2_web_platform_t *)user)->fake_network;
  pthread_mutex_lock(&s->mutex);
  const bool open = s->modem_open, registered = modem_registered(s, &e),
             data = registered && s->data_open;
  pthread_mutex_unlock(&s->mutex);
  if (!open)
    return H2_PAL_ERR_INVALID_STATE;
  (void)modem_capabilities(user, &out->capabilities);
  out->sim = e.flags[H2_WEB_FAKE_SIM_PRESENT] ? H2_PAL_MODEM_SIM_STATE_READY
                                              : H2_PAL_MODEM_SIM_STATE_ABSENT;
  out->registration = registered ? H2_PAL_MODEM_REGISTRATION_HOME
                                 : H2_PAL_MODEM_REGISTRATION_OFFLINE;
  out->packet = data         ? H2_PAL_MODEM_PACKET_CONNECTED
                : registered ? H2_PAL_MODEM_PACKET_ATTACHED
                             : H2_PAL_MODEM_PACKET_DETACHED;
  out->rat = H2_PAL_MODEM_RAT_LTE;
  return H2_PAL_OK;
}
static h2_pal_result_t modem_identity(void *user,
                                      h2_pal_modem_identity_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  h2_pal_modem_status_t status;
  int rc = modem_status(user, &status);
  if (rc != H2_PAL_OK)
    return rc;
  memcpy(out->manufacturer, "GizOS simulation", 17);
  memcpy(out->model, "Web fake modem", 15);
  memcpy(out->revision, "1", 2);
  /* No fabricated subscriber or manufacturing identifiers. */
  return H2_PAL_OK;
}
static h2_pal_result_t modem_operator(void *user,
                                      h2_pal_modem_operator_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  h2_pal_modem_status_t status;
  int rc = modem_status(user, &status);
  if (rc != H2_PAL_OK)
    return rc;
  memcpy(out->name, "Browser (simulated)", 20);
  out->rat = H2_PAL_MODEM_RAT_LTE;
  return H2_PAL_OK;
}
static h2_pal_result_t modem_apn(void *user,
                                 const h2_pal_modem_apn_config_t *config) {
  if (!config || !memchr(config->apn, 0, sizeof(config->apn)) ||
      !memchr(config->username, 0, sizeof(config->username)) ||
      !memchr(config->password, 0, sizeof(config->password)))
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_modem_status_t status;
  return modem_status(user, &status);
}
static h2_pal_result_t data_open(void *user, uint32_t timeout) {
  (void)timeout;
  environment_t e;
  int rc = snapshot(user, &e);
  if (rc != H2_PAL_OK)
    return rc;
  h2_web_platform_t *p = user;
  h2_web_fake_network_t *s = p->fake_network;
  pthread_mutex_lock(&s->mutex);
  rc = !s->modem_open             ? H2_PAL_ERR_INVALID_STATE
       : !modem_registered(s, &e) ? H2_PAL_ERR_UNAVAILABLE
                                  : H2_PAL_OK;
  if (rc == H2_PAL_OK)
    s->data_open = true;
  pthread_mutex_unlock(&s->mutex);
  changed(p);
  return rc;
}
static h2_pal_result_t data_close(void *user, uint32_t timeout) {
  (void)timeout;
  h2_web_platform_t *p = user;
  h2_web_fake_network_t *s = p->fake_network;
  pthread_mutex_lock(&s->mutex);
  s->data_open = false;
  pthread_mutex_unlock(&s->mutex);
  changed(p);
  return H2_PAL_OK;
}
static h2_pal_result_t data_status(void *user,
                                   h2_pal_modem_data_status_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  environment_t e;
  int rc = snapshot(user, &e);
  if (rc != H2_PAL_OK)
    return rc;
  h2_web_fake_network_t *s = ((h2_web_platform_t *)user)->fake_network;
  pthread_mutex_lock(&s->mutex);
  const bool open = s->modem_open,
             connected = modem_registered(s, &e) && s->data_open;
  pthread_mutex_unlock(&s->mutex);
  if (!open)
    return H2_PAL_ERR_INVALID_STATE;
  out->state = connected ? H2_PAL_MODEM_DATA_OPEN : H2_PAL_MODEM_DATA_CLOSED;
  out->last_error = connected ? H2_PAL_OK : H2_PAL_ERR_UNAVAILABLE;
  if (connected) {
    out->ip4 = 0xc0000264u;
    out->ip4_valid = 1;
  }
  return H2_PAL_OK;
}
static h2_pal_result_t modem_signal(void *user, h2_pal_modem_signal_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  environment_t e;
  int rc = snapshot(user, &e);
  if (rc != H2_PAL_OK)
    return rc;
  h2_pal_modem_status_t status;
  rc = modem_status(user, &status);
  if (rc != H2_PAL_OK)
    return rc;
  out->rat = H2_PAL_MODEM_RAT_LTE;
  if (status.registration == H2_PAL_MODEM_REGISTRATION_HOME) {
    out->rssi_valid = 1;
    out->rssi_dbm = e.flags[H2_WEB_FAKE_MODEM_RSSI];
  }
  return H2_PAL_OK;
}
static h2_pal_result_t modem_policy(void *user,
                                    h2_pal_modem_power_policy_t policy) {
  if (policy != H2_PAL_MODEM_POWER_POLICY_ACTIVE &&
      policy != H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP)
    return H2_PAL_ERR_INVALID_ARG;
  h2_web_fake_network_t *s = ((h2_web_platform_t *)user)->fake_network;
  pthread_mutex_lock(&s->mutex);
  int rc = s->modem_open ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK)
    s->policy = policy;
  pthread_mutex_unlock(&s->mutex);
  return rc;
}
static h2_pal_result_t modem_power(void *user,
                                   h2_pal_modem_power_status_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  h2_web_fake_network_t *s = ((h2_web_platform_t *)user)->fake_network;
  pthread_mutex_lock(&s->mutex);
  int rc = s->modem_open ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) {
    out->policy = s->policy;
    out->state =
        s->policy == H2_PAL_MODEM_POWER_POLICY_AUTO_SLEEP && !s->data_open
            ? H2_PAL_MODEM_POWER_STATE_ASLEEP
            : H2_PAL_MODEM_POWER_STATE_ACTIVE;
  }
  pthread_mutex_unlock(&s->mutex);
  return rc;
}

static const h2_pal_wifi_sta_vtable_t wifi_vtable = {
    .get_status = wifi_status,
    .scan = wifi_scan,
    .connect = wifi_connect,
    .connect_and_save = wifi_connect_save,
    .disconnect = wifi_disconnect,
    .get_mac = wifi_mac,
    .set_power_save = wifi_power_save,
};
static const h2_pal_wifi_settings_vtable_t settings_vtable = {
    .get_saved_sta_config = saved_get,
    .set_saved_sta_config = saved_set,
    .clear_saved_sta_config = saved_clear,
    .has_saved_sta_config = saved_has,
};
static const h2_pal_modem_vtable_t modem_vtable = {
    .open = modem_open,
    .close = modem_close,
    .get_capabilities = modem_capabilities,
    .get_status = modem_status,
    .get_identity = modem_identity,
    .get_operator = modem_operator,
    .set_apn = modem_apn,
    .data_open = data_open,
    .data_close = data_close,
    .get_data_status = data_status,
    .get_signal = modem_signal,
    .set_power_policy = modem_policy,
    .get_power_status = modem_power,
};

h2_pal_result_t h2_web_platform_fake_network_init(h2_web_platform_t *p) {
  environment_t e;
  int enabled = environment_read(&e);
  if (enabled == 0)
    return H2_PAL_OK;
  if (enabled != 1)
    return H2_PAL_ERR_INVALID_ARG;
  h2_web_fake_network_t *s = calloc(1, sizeof(*s));
  if (!s)
    return H2_PAL_ERR_NO_MEMORY;
  if (pthread_mutex_init(&s->mutex, NULL) != 0) {
    free(s);
    return H2_PAL_ERR_NO_MEMORY;
  }
  s->wifi = (h2_pal_wifi_sta_api_t){p, &wifi_vtable};
  s->settings = (h2_pal_wifi_settings_api_t){p, &settings_vtable};
  s->modem = (h2_pal_modem_api_t){p, &modem_vtable};
  s->wifi_connected = true;
  s->saved_valid = true;
  memcpy(s->saved.ssid, e.ssid, sizeof(s->saved.ssid));
  s->saved.ssid_len = strlen(e.ssid);
  p->fake_network = s;
  return H2_PAL_OK;
}
void h2_web_platform_fake_network_deinit(h2_web_platform_t *p) {
  h2_web_fake_network_t *s = p->fake_network;
  if (!s)
    return;
  pthread_mutex_destroy(&s->mutex);
  memset(s, 0, sizeof(*s));
  free(s);
  p->fake_network = NULL;
}
const h2_pal_wifi_sta_api_t *
h2_web_platform_fake_wifi_sta_api(h2_web_platform_t *p) {
  h2_web_fake_network_t *s = p ? p->fake_network : NULL;
  return s ? &s->wifi : NULL;
}
const h2_pal_wifi_settings_api_t *
h2_web_platform_fake_wifi_settings_api(h2_web_platform_t *p) {
  h2_web_fake_network_t *s = p ? p->fake_network : NULL;
  return s ? &s->settings : NULL;
}
const h2_pal_modem_api_t *h2_web_platform_fake_modem_api(h2_web_platform_t *p) {
  h2_web_fake_network_t *s = p ? p->fake_network : NULL;
  return s ? &s->modem : NULL;
}
