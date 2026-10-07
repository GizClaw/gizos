#include "h2_iperf_client_app.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef enum failure {
  FAILURE_NONE,
  FAILURE_CONNECT,
  FAILURE_POWER,
  FAILURE_TIME,
  FAILURE_POLL,
  FAILURE_STATUS,
  FAILURE_STATE,
  FAILURE_ADDRESS,
  FAILURE_SLEEP,
  FAILURE_TIMEOUT,
  FAILURE_SAVED_BEFORE,
  FAILURE_SAVED_AFTER,
  FAILURE_SAVED_CHANGED,
} failure_t;

typedef struct fixture {
  failure_t failure;
  h2_pal_wifi_sta_status_t wifi;
  uint64_t now;
  unsigned connects;
  unsigned disconnects;
  unsigned saved_reads;
  unsigned matrix_calls;
  unsigned complete_records;
  char complete[384];
  char signature_hex[65];
  failure_t secondary_saved;
  int disconnect_error;
  int matrix_error;
  int pending_event;
  int associated;
  h2_iperf_client_app_mode_t mode;
  int other_interface;
} fixture_t;

static int connect_wifi(void *user, const h2_pal_wifi_sta_config_t *config,
                        uint32_t timeout_ms) {
  fixture_t *f = user;
  assert(timeout_ms == 20000u);
  assert(config->ssid_len <= sizeof(f->wifi.ssid));
  f->associated = 1;
  ++f->connects;
  f->wifi.state = H2_PAL_WIFI_STA_STATE_GOT_IP;
  f->wifi.ssid_len = config->ssid_len;
  memcpy(f->wifi.ssid, config->ssid, config->ssid_len);
  f->wifi.ip_valid = f->failure != FAILURE_TIMEOUT &&
                     f->mode != H2_IPERF_CLIENT_APP_IPV6;
  f->wifi.ip.ip4 = f->wifi.ip_valid ? 0xc0a80402u : 0u;
  f->wifi.ip.ip6_valid = f->failure != FAILURE_TIMEOUT &&
      (f->mode == H2_IPERF_CLIENT_APP_IPV6 ||
       f->mode == H2_IPERF_CLIENT_APP_DUAL);
  if (f->wifi.ip.ip6_valid) {
    const uint8_t address[16] = {0xfd, 0x53, 0x69, 0x7a, 0x6f, 0x73,
                                0x06, 0x26, 0, 0, 0, 0, 0, 0, 0, 2};
    memcpy(f->wifi.ip.ip6, address, sizeof(address));
  }
  f->pending_event = H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP;
  return f->failure == FAILURE_CONNECT ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static int disconnect_wifi(void *user) {
  fixture_t *f = user;
  ++f->disconnects;
  if (f->disconnect_error != H2_PAL_OK)
    return f->disconnect_error;
  f->associated = 0;
  f->wifi.state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
  f->wifi.ip_valid = 0u;
  memset(&f->wifi.ip, 0, sizeof(f->wifi.ip));
  f->pending_event = H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_LOST_IP;
  return H2_PAL_OK;
}

static int power_save(void *user, h2_pal_wifi_power_save_t mode) {
  fixture_t *f = user;
  assert(mode == H2_PAL_WIFI_POWER_SAVE_NONE);
  return f->failure == FAILURE_POWER ? H2_PAL_ERR_IO : H2_PAL_OK;
}

static int wifi_status(void *user, h2_pal_wifi_sta_status_t *out) {
  fixture_t *f = user;
  if (f->failure == FAILURE_STATUS)
    return H2_PAL_ERR_IO;
  *out = f->wifi;
  return H2_PAL_OK;
}

static h2_pal_result_t monotonic(void *user, uint64_t *out) {
  fixture_t *f = user;
  if (f->failure == FAILURE_TIME)
    return H2_PAL_ERR_IO;
  *out = f->now;
  return H2_PAL_OK;
}

static h2_pal_result_t sleep_ms(void *user, uint32_t ms) {
  fixture_t *f = user;
  if (f->failure == FAILURE_SLEEP)
    return H2_PAL_ERR_IO;
  f->now += ms;
  return H2_PAL_OK;
}

h2_pal_result_t h2_runtime_poll_event(h2_runtime_t *runtime,
                                     h2_runtime_event_t *event) {
  fixture_t *f = runtime->wifi_sta->user;
  if (f->failure == FAILURE_POLL)
    return H2_PAL_ERR_IO;
  if (!f->pending_event)
    return H2_PAL_ERR_WOULD_BLOCK;
  assert((uintptr_t)event->payload %
             _Alignof(h2_runtime_system_event_wifi_sta_t) == 0u);
  h2_runtime_system_event_wifi_sta_t payload = {0};
  payload.ssid_len = f->wifi.ssid_len;
  memcpy(payload.ssid, f->wifi.ssid, f->wifi.ssid_len);
  payload.ip_valid = f->wifi.ip_valid;
  payload.ip.ip4 = f->wifi.ip.ip4;
  payload.ip.ip6_valid = f->wifi.ip.ip6_valid;
  memcpy(payload.ip.ip6, f->wifi.ip.ip6, sizeof(payload.ip.ip6));
  assert(event->payload_capacity >= sizeof(payload));
  memcpy(event->payload, &payload, sizeof(payload));
  event->payload_size = sizeof(payload);
  event->kind = f->pending_event;
  f->pending_event = 0;
  return H2_PAL_OK;
}

h2_pal_result_t h2_runtime_system_state_wifi_sta(
    const h2_runtime_t *runtime, h2_runtime_system_wifi_sta_state_t *out) {
  fixture_t *f = runtime->wifi_sta->user;
  if (f->failure == FAILURE_STATE)
    return H2_PAL_ERR_IO;
  memset(out, 0, sizeof(*out));
  out->valid = 1u;
  out->status = f->associated ? H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_GOT_IP
                              : H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_DISCONNECTED;
  out->ssid_len = f->wifi.ssid_len;
  memcpy(out->ssid, f->wifi.ssid, f->wifi.ssid_len);
  out->ip_valid = f->wifi.ip_valid;
  out->ip.ip4 = f->wifi.ip.ip4;
  out->ip.ip6_valid = f->wifi.ip.ip6_valid;
  memcpy(out->ip.ip6, f->wifi.ip.ip6, sizeof(out->ip.ip6));
  return H2_PAL_OK;
}

static int host_address(void *user, const char *prefix,
                         h2_pal_net_family_t family, h2_pal_net_addr_t *out) {
  fixture_t *f = user;
  assert(prefix == NULL && family == H2_PAL_NET_FAMILY_IPV4);
  out->family = family;
  h2_pal_wifi_ip4_to_bytes(f->wifi.ip.ip4, out->ip);
  if (f->other_interface) {
    const uint8_t ethernet[4] = {10, 0, 0, 2};
    memcpy(out->ip, ethernet, sizeof(ethernet));
  }
  if (f->failure == FAILURE_ADDRESS)
    out->ip[3] ^= 1u;
  return H2_PAL_OK;
}

static h2_pal_result_t netif_status(void *user, const h2_pal_netif_ref_t *ref,
                                     h2_pal_netif_status_t *out) {
  fixture_t *f = user;
  assert(ref->kind == H2_PAL_NETIF_KIND_WIFI_STA);
  assert(ref->type == H2_PAL_NETIF_REF_KIND);
  memset(out, 0, sizeof(*out));
  out->kind = H2_PAL_NETIF_KIND_WIFI_STA;
  if (f->associated) {
    out->flags = H2_PAL_NETIF_FLAG_UP | H2_PAL_NETIF_FLAG_LINK_UP;
    if (f->wifi.ip_valid) {
      out->flags |= H2_PAL_NETIF_FLAG_HAS_IPV4;
      out->ipv4.family = H2_PAL_NET_FAMILY_IPV4;
      h2_pal_wifi_ip4_to_bytes(f->wifi.ip.ip4, out->ipv4.ip);
      if (f->failure == FAILURE_ADDRESS)
        out->ipv4.ip[3] ^= 1u;
    }
    if (f->wifi.ip.ip6_valid) {
      out->flags |= H2_PAL_NETIF_FLAG_HAS_IPV6;
      out->ipv6.family = H2_PAL_NET_FAMILY_IPV6;
      memcpy(out->ipv6.ip, f->wifi.ip.ip6, sizeof(out->ipv6.ip));
      if (f->failure == FAILURE_ADDRESS)
        out->ipv6.ip[15] ^= 1u;
    }
  }
  return H2_PAL_OK;
}

static int get_saved(void *user, h2_pal_wifi_sta_config_t *out) {
  fixture_t *f = user;
  memset(out, 0, sizeof(*out));
  ++f->saved_reads;
  if ((f->failure == FAILURE_SAVED_BEFORE && f->saved_reads == 1u) ||
      ((f->failure == FAILURE_SAVED_AFTER ||
        f->secondary_saved == FAILURE_SAVED_AFTER) && f->saved_reads == 2u))
    return H2_PAL_ERR_IO;
  if (f->failure == FAILURE_SAVED_CHANGED ||
      f->secondary_saved == FAILURE_SAVED_CHANGED) {
    const char *ssid = f->saved_reads == 1u ? "home" : "away";
    memcpy(out->ssid, ssid, 4u);
    out->ssid_len = 4u;
    memcpy(out->password, "private-secret", 14u);
    out->password_len = 14u;
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t signature(void *user, const uint8_t *secret,
                                 size_t secret_len, const uint8_t *salt,
                                 size_t salt_len, const uint8_t *info,
                                 size_t info_len, uint8_t *out, size_t out_len) {
  fixture_t *f = user;
  assert(secret_len == 108u && out_len == 32u);
  (void)salt;
  (void)salt_len;
  (void)info;
  (void)info_len;
  /* Inject deterministic digest bytes; the equality check still observes the
   * actual canonical saved-config record built by the production helper. */
  memset(out, 0, out_len);
  for (size_t i = 0u; i < secret_len; ++i)
    out[i % out_len] ^= secret[i];
  for (size_t i = 0u; i < out_len; ++i)
    (void)snprintf(f->signature_hex + i * 2u, 3u, "%02x", out[i]);
  return H2_PAL_OK;
}

/* Inject the matrix outcome; the separate client_test exchanges real data. */
h2_pal_result_t h2_iperf_client_app_run(
    h2_runtime_t *runtime, const h2_iperf_client_app_config_t *config,
    h2_iperf_client_app_report_t *out) {
  fixture_t *f = runtime->wifi_sta->user;
  assert(f->associated && config->mode == H2_IPERF_CLIENT_APP_IPV4);
  ++f->matrix_calls;
  out->total = 30u;
  out->passed = f->matrix_error ? 29u : 30u;
  return f->matrix_error;
}

static int log_record(void *user, h2_pal_log_level_t level, const char *scope,
                       const char *message) {
  fixture_t *f = user;
  assert(level == H2_PAL_LOG_INFO && strcmp(scope, "iperf-client") == 0);
  assert(strstr(message, "private-secret") == NULL);
  if (f->signature_hex[0])
    assert(strstr(message, f->signature_hex) == NULL);
  assert(strstr(message, "CONFIRMED") == NULL);
  if (strstr(message, "H2_IPERF_CLIENT_COMPLETE ") == message) {
    ++f->complete_records;
    assert(strlen(message) < sizeof(f->complete));
    strcpy(f->complete, message);
  }
  return H2_PAL_OK;
}

static int result_field(const char *line, const char *name) {
  const char *start = strstr(line, name);
  assert(start != NULL);
  int value = 0;
  assert(sscanf(start + strlen(name), "%d", &value) == 1);
  return value;
}

static void test_case_impl(failure_t failure, int matrix_error,
                           int disconnect_error, int expected, int bench,
                           failure_t secondary_saved,
                           h2_iperf_client_app_mode_t mode, int other_interface) {
  fixture_t f = {.failure = failure,
                 .matrix_error = matrix_error,
                 .disconnect_error = disconnect_error,
                 .secondary_saved = secondary_saved,
                 .mode = mode, .other_interface = other_interface};
  const h2_pal_wifi_sta_vtable_t wifi_vtable = {
      .connect = connect_wifi,
      .disconnect = disconnect_wifi,
      .get_status = wifi_status,
      .set_power_save = power_save};
  const h2_pal_wifi_sta_api_t wifi = {&f, &wifi_vtable};
  const h2_pal_time_vtable_t time_vtable = {
      .get_monotonic_ms = monotonic, .sleep_ms = sleep_ms};
  const h2_pal_time_api_t time = {&f, &time_vtable};
  const h2_pal_net_vtable_t net_vtable = {.get_host_addr_family = host_address};
  const h2_pal_net_api_t net = {&f, &net_vtable};
  const h2_pal_netif_vtable_t netif_vtable = {.get_status = netif_status};
  const h2_pal_netif_api_t netif = {&f, &netif_vtable};
  const h2_pal_wifi_settings_vtable_t settings_vtable = {
      .get_saved_sta_config = get_saved};
  const h2_pal_wifi_settings_api_t settings = {&f, &settings_vtable};
  const h2_pal_crypto_vtable_t crypto_vtable = {.hkdf_sha256 = signature};
  const h2_pal_crypto_api_t crypto = {&f, &crypto_vtable};
  const h2_pal_log_vtable_t log_vtable = {.write = log_record};
  const h2_pal_log_api_t log = {&f, &log_vtable};
  h2_runtime_t runtime = {.wifi_sta = &wifi,
                          .time = &time,
                          .net = &net,
                          .netif = &netif,
                          .wifi_settings = &settings,
                          .crypto = &crypto,
                          .log = &log};
  int rc;
  if (bench) {
    rc = h2_iperf_client_app_bench(&runtime, "test", NULL, NULL);
  } else {
    const h2_pal_wifi_sta_config_t config = {.ssid = "test", .ssid_len = 4u};
    h2_iperf_client_app_network_t network;
    memset(&network, 0xa5, sizeof(network));
    rc = h2_iperf_client_app_connect(&runtime, &config, &network);
    if (expected != H2_PAL_OK) {
      const h2_iperf_client_app_network_t empty = {0};
      assert(memcmp(&network, &empty, sizeof(empty)) == 0);
    } else {
      assert(network.runtime_ready && network.mode == mode);
    }
  }
  assert(rc == expected);
  if (bench) {
    assert(f.saved_reads == 2u);
    assert(f.complete_records == 1u);
    assert(result_field(f.complete, " rc=") == expected);
    assert(result_field(f.complete, " matrix_started=") == (int)f.matrix_calls);
    if (f.matrix_calls == 0u) {
      assert(result_field(f.complete, " total=") == 0);
      assert(result_field(f.complete, " passed=") == 0);
      assert(result_field(f.complete, " matrix_rc=") == H2_PAL_ERR_INVALID_STATE);
    }
    const char *gates[] = {" public_wifi=", " runtime_ip=",
                           " disconnect_reconnect=", " saved_unchanged="};
    for (unsigned i = 0u; i < sizeof(gates) / sizeof(gates[0]); ++i)
      assert(result_field(f.complete, gates[i]) == (expected == H2_PAL_OK));
    int saved_expected = failure == FAILURE_SAVED_AFTER ||
                         secondary_saved == FAILURE_SAVED_AFTER ? H2_PAL_ERR_IO
        : failure == FAILURE_SAVED_BEFORE || failure == FAILURE_SAVED_CHANGED ||
                  secondary_saved == FAILURE_SAVED_CHANGED ? H2_PAL_ERR_INVALID_STATE
                                                          : H2_PAL_OK;
    assert(result_field(f.complete, " saved_check_rc=") == saved_expected);
  } else {
    assert(f.saved_reads == 0u && f.complete_records == 0u);
  }
  if (expected == H2_PAL_OK) {
    assert(f.associated);
    assert(f.disconnects == (bench ? 1u : 0u));
  } else {
    assert(f.disconnects >= (failure == FAILURE_SAVED_BEFORE ? 0u : 1u));
    assert(disconnect_error != H2_PAL_OK || !f.associated);
  }
  if (failure == FAILURE_SAVED_BEFORE)
    assert(f.connects == 0u);
  else if (!bench || failure <= FAILURE_TIMEOUT || disconnect_error != H2_PAL_OK)
    assert(f.connects == (failure == FAILURE_NONE && bench &&
                         disconnect_error == H2_PAL_OK ? 2u : 1u));
  else
    assert(f.connects == 2u);
}

static void test_case(failure_t failure, int matrix_error,
                      int disconnect_error, int expected, int bench) {
  test_case_impl(failure, matrix_error, disconnect_error, expected, bench,
                 FAILURE_NONE, H2_IPERF_CLIENT_APP_IPV4, 0);
}

int main(void) {
  test_case(FAILURE_NONE, H2_PAL_OK, H2_PAL_OK, H2_PAL_OK, 0);
  for (failure_t f = FAILURE_CONNECT; f <= FAILURE_TIMEOUT; ++f) {
    int expected = f == FAILURE_ADDRESS ? H2_PAL_ERR_INVALID_STATE
                   : f == FAILURE_TIMEOUT ? H2_PAL_ERR_TIMEOUT
                                          : H2_PAL_ERR_IO;
    test_case(f, H2_PAL_OK, H2_PAL_OK, expected, 0);
    test_case(f, H2_PAL_OK, H2_PAL_OK, expected, 1);
  }
  test_case(FAILURE_POWER, H2_PAL_OK, H2_PAL_ERR_BUSY, H2_PAL_ERR_IO, 0);
  test_case(FAILURE_NONE, H2_PAL_OK, H2_PAL_OK, H2_PAL_OK, 1);
  test_case(FAILURE_NONE, H2_PAL_ERR_IO, H2_PAL_OK, H2_PAL_ERR_IO, 1);
  test_case(FAILURE_SAVED_AFTER, H2_PAL_OK, H2_PAL_OK, H2_PAL_ERR_IO, 1);
  test_case(FAILURE_SAVED_BEFORE, H2_PAL_OK, H2_PAL_OK, H2_PAL_ERR_IO, 1);
  test_case(FAILURE_SAVED_CHANGED, H2_PAL_OK, H2_PAL_OK, H2_PAL_ERR_INVALID_STATE, 1);
  test_case_impl(FAILURE_CONNECT, H2_PAL_OK, H2_PAL_OK, H2_PAL_ERR_IO, 1,
                 FAILURE_SAVED_CHANGED, H2_IPERF_CLIENT_APP_IPV4, 0);
  test_case_impl(FAILURE_TIMEOUT, H2_PAL_OK, H2_PAL_OK, H2_PAL_ERR_TIMEOUT, 1,
                 FAILURE_SAVED_AFTER, H2_IPERF_CLIENT_APP_IPV4, 0);
  const h2_iperf_client_app_mode_t modes[] = {H2_IPERF_CLIENT_APP_IPV4,
      H2_IPERF_CLIENT_APP_IPV6, H2_IPERF_CLIENT_APP_DUAL};
  for (unsigned i = 0u; i < sizeof(modes) / sizeof(modes[0]); ++i) {
    test_case_impl(FAILURE_NONE, H2_PAL_OK, H2_PAL_OK, H2_PAL_OK, 0,
                   FAILURE_NONE, modes[i], 1);
    test_case_impl(FAILURE_ADDRESS, H2_PAL_OK, H2_PAL_OK,
                   H2_PAL_ERR_INVALID_STATE, 0, FAILURE_NONE, modes[i], 1);
  }
  test_case(FAILURE_NONE, H2_PAL_ERR_IO, H2_PAL_ERR_BUSY, H2_PAL_ERR_IO, 1);
  puts("iperf-client network PASS: aligned events and failure cleanup");
  return 0;
}
