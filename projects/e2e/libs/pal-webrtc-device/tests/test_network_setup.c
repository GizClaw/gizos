#include "h2_pal_webrtc_network.h"
#include <string.h>

typedef struct fixture {
  h2_pal_wifi_sta_status_t current, connected;
  int saved_result, connect_result;
  uint64_t now_ms;
  unsigned saved_reads, connects, saved_writes, saved_clears;
} fixture_t;

static int status(void *user, h2_pal_wifi_sta_status_t *out) {
  *out = ((fixture_t *)user)->current;
  return H2_PAL_OK;
}

static int saved(void *user, h2_pal_wifi_sta_config_t *out) {
  fixture_t *fixture = user;
  ++fixture->saved_reads;
  memset(out, 0, sizeof(*out));
  memcpy(out->ssid, "fixture", 7u);
  out->ssid_len = 7u;
  return fixture->saved_result;
}

static int save(void *user, const h2_pal_wifi_sta_config_t *config) {
  (void)config;
  ++((fixture_t *)user)->saved_writes;
  return H2_PAL_ERR_INVALID_STATE;
}

static int clear(void *user) {
  ++((fixture_t *)user)->saved_clears;
  return H2_PAL_ERR_INVALID_STATE;
}

static int connect(void *user, const h2_pal_wifi_sta_config_t *config,
                   uint32_t timeout_ms) {
  fixture_t *fixture = user;
  ++fixture->connects;
  if (config->ssid_len != 7u || timeout_ms != 20000u)
    return H2_PAL_ERR_INVALID_ARG;
  fixture->current = fixture->connected;
  return fixture->connect_result;
}

static int clock_ms(void *user, uint64_t *out) {
  *out = ((fixture_t *)user)->now_ms;
  return H2_PAL_OK;
}

static int sleep_ms(void *user, uint32_t duration_ms) {
  ((fixture_t *)user)->now_ms += duration_ms;
  return H2_PAL_OK;
}

static int prepare(fixture_t *fixture) {
  const h2_pal_wifi_sta_vtable_t sta_methods = {.get_status = status,
                                              .connect = connect};
  const h2_pal_wifi_settings_vtable_t settings_methods = {
      .get_saved_sta_config = saved,
      .set_saved_sta_config = save,
      .clear_saved_sta_config = clear};
  const h2_pal_time_vtable_t time_methods = {.get_monotonic_ms = clock_ms,
                                           .sleep_ms = sleep_ms};
  const h2_pal_wifi_sta_api_t sta = {fixture, &sta_methods};
  const h2_pal_wifi_settings_api_t settings = {fixture, &settings_methods};
  const h2_pal_time_api_t time = {fixture, &time_methods};
  h2_runtime_t runtime = {.wifi_sta = &sta,
                          .wifi_settings = &settings,
                          .time = &time};
  int rc = h2_pal_webrtc_device_prepare_network(&runtime);
  return fixture->saved_writes || fixture->saved_clears
             ? H2_PAL_ERR_INVALID_STATE
             : rc;
}

int main(void) {
  const h2_pal_wifi_sta_status_t ipv4 = {
      .state = H2_PAL_WIFI_STA_STATE_GOT_IP,
      .ip_valid = 1,
      .ip = {.ip4 = 1u}};
  const h2_pal_wifi_sta_status_t ipv6 = {
      .state = H2_PAL_WIFI_STA_STATE_GOT_IP,
      .ip = {.ip6_valid = 1, .ip6 = {0xfd, 1u}}};
  fixture_t fixture = {.current = ipv4,
                       .saved_result = H2_PAL_ERR_NOT_FOUND};
  if (prepare(&fixture) != H2_PAL_OK || fixture.saved_reads || fixture.connects)
    return 1;
  fixture = (fixture_t){.current = ipv6,
                        .saved_result = H2_PAL_ERR_NOT_FOUND};
  if (prepare(&fixture) != H2_PAL_OK || fixture.saved_reads || fixture.connects)
    return 2;
  fixture = (fixture_t){.connected = ipv6};
  if (prepare(&fixture) != H2_PAL_OK || fixture.saved_reads != 1u ||
      fixture.connects != 1u)
    return 3;
  fixture = (fixture_t){.saved_result = H2_PAL_ERR_NOT_FOUND};
  if (prepare(&fixture) != H2_PAL_ERR_NOT_FOUND || fixture.connects)
    return 4;
  /* Provisioning can satisfy the prerequisite later without saving a profile. */
  fixture.current = ipv4;
  if (prepare(&fixture) != H2_PAL_OK || fixture.saved_reads != 1u ||
      fixture.connects)
    return 5;
  fixture = (fixture_t){.current = ipv6,
                        .saved_result = H2_PAL_ERR_NOT_FOUND};
  fixture.current.ip.ip6[0] = 0xfeu;
  fixture.current.ip.ip6[1] = 0x80u;
  if (prepare(&fixture) != H2_PAL_ERR_NOT_FOUND || fixture.saved_reads != 1u)
    return 6;
  fixture = (fixture_t){.connected = ipv4,
                        .connect_result = H2_PAL_ERR_BUSY};
  if (prepare(&fixture) != H2_PAL_ERR_BUSY)
    return 7;
  fixture = (fixture_t){0};
  if (prepare(&fixture) != H2_PAL_ERR_TIMEOUT || fixture.now_ms != 20000u)
    return 8;
  return h2_pal_webrtc_device_prepare_network(NULL) == H2_PAL_ERR_INVALID_ARG
             ? 0
             : 9;
}
