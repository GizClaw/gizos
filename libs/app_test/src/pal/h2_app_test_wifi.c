#include "h2_app_test_wifi.h"
#include <string.h>
static bool valid(const h2_pal_wifi_sta_config_t *c) {
  return c && c->ssid_len > 0 && c->ssid_len <= H2_PAL_WIFI_SSID_MAX &&
         c->password_len <= H2_PAL_WIFI_PASSWORD_MAX;
}
static int status(void *u, h2_pal_wifi_sta_status_t *out) {
  h2_app_test_wifi_t *w = u;
  memset(out, 0, sizeof(*out));
  int rc = h2_app_test_fault_take(&w->get_status);
  if (!rc) {
    if (w->status.ssid_len > H2_PAL_WIFI_SSID_MAX)
      return H2_PAL_ERR_FORMAT;
    *out = w->status;
  }
  return rc;
}
static int scan(void *u, const h2_pal_wifi_scan_request_t *r,
                h2_pal_wifi_scan_result_fn cb, void *cu, uint32_t timeout) {
  h2_app_test_wifi_t *w = u;
  if (!cb || (r && r->ssid_len > H2_PAL_WIFI_SSID_MAX))
    return H2_PAL_ERR_INVALID_ARG;
  if (w->entry_count > H2_PAL_WIFI_SCAN_MAX_RESULTS)
    return H2_PAL_ERR_FORMAT;
  for (size_t i = 0; i < w->entry_count; ++i)
    if (w->entries[i].ssid_len > H2_PAL_WIFI_SSID_MAX)
      return H2_PAL_ERR_FORMAT;
  w->last_scan = r ? *r : (h2_pal_wifi_scan_request_t){0};
  w->last_timeout_ms = timeout;
  int rc = h2_app_test_fault_take(&w->scan);
  if (rc)
    return rc;
  for (size_t i = 0; i < w->entry_count; ++i) {
    const h2_pal_wifi_scan_entry_t *e = &w->entries[i];
    if (r && ((r->channel && r->channel != e->channel) ||
              (r->ssid_len && (r->ssid_len != e->ssid_len ||
                               memcmp(r->ssid, e->ssid, r->ssid_len)))))
      continue;
    if (!cb(cu, e))
      break;
  }
  return H2_PAL_OK;
}
static int connect(void *u, const h2_pal_wifi_sta_config_t *c,
                   uint32_t timeout) {
  h2_app_test_wifi_t *w = u;
  if (!valid(c))
    return H2_PAL_ERR_INVALID_ARG;
  w->last_connect = *c;
  w->last_timeout_ms = timeout;
  return h2_app_test_fault_take(&w->connect);
}
static int disconnect(void *u) {
  h2_app_test_wifi_t *w = u;
  int rc = h2_app_test_fault_take(&w->disconnect);
  if (!rc)
    w->status = (h2_pal_wifi_sta_status_t){.state = H2_PAL_WIFI_STA_STATE_IDLE};
  return rc;
}
static int get_saved(void *u, h2_pal_wifi_sta_config_t *out) {
  h2_app_test_wifi_t *w = u;
  memset(out, 0, sizeof(*out));
  int rc = h2_app_test_fault_take(&w->get_saved);
  if (rc)
    return rc;
  if (!w->saved_present)
    return H2_PAL_ERR_NOT_FOUND;
  *out = w->saved;
  return 0;
}
static int set_saved(void *u, const h2_pal_wifi_sta_config_t *c) {
  h2_app_test_wifi_t *w = u;
  if (!valid(c))
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_app_test_fault_take(&w->set_saved);
  if (!rc) {
    w->saved = *c;
    w->saved_present = true;
  }
  return rc;
}
static int clear_saved(void *u) {
  h2_app_test_wifi_t *w = u;
  int rc = h2_app_test_fault_take(&w->clear_saved);
  if (!rc) {
    memset(&w->saved, 0, sizeof(w->saved));
    w->saved_present = false;
  }
  return rc;
}
static const h2_pal_wifi_sta_vtable_t vtable = {.get_status = status,
                                                .scan = scan,
                                                .connect = connect,
                                                .disconnect = disconnect};
static const h2_pal_wifi_settings_vtable_t settings = {
    .get_saved_sta_config = get_saved,
    .set_saved_sta_config = set_saved,
    .clear_saved_sta_config = clear_saved};
void h2_app_test_wifi_init(h2_app_test_wifi_t *w) {
  if (!w)
    return;
  memset(w, 0, sizeof(*w));
  w->api = (h2_pal_wifi_sta_api_t){w, &vtable};
  w->settings = (h2_pal_wifi_settings_api_t){w, &settings};
  w->status.state = H2_PAL_WIFI_STA_STATE_IDLE;
}
