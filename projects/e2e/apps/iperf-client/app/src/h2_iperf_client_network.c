#include "h2_iperf_client_app.h"
#include <stdio.h>
#include <string.h>

static int same_addresses(const h2_pal_wifi_sta_status_t *pal,
                          const h2_runtime_system_wifi_sta_state_t *state) {
  return state->valid &&
         state->status == H2_RUNTIME_SYSTEM_WIFI_STA_STATUS_GOT_IP &&
         state->ssid_len == pal->ssid_len &&
         memcmp(state->ssid, pal->ssid, pal->ssid_len) == 0 &&
         state->ip_valid == pal->ip_valid && state->ip.ip4 == pal->ip.ip4 &&
         state->ip.ip6_valid == pal->ip.ip6_valid &&
         memcmp(state->ip.ip6, pal->ip.ip6, sizeof(pal->ip.ip6)) == 0;
}

static int saved_signature(h2_runtime_t *runtime, uint8_t out[32]) {
  h2_pal_wifi_sta_config_t saved = {0};
  int rc =
      h2_pal_wifi_settings_get_saved_sta_config(runtime->wifi_settings, &saved);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND)
    return rc;
  if (rc == H2_PAL_ERR_NOT_FOUND)
    memset(&saved, 0, sizeof(saved));
  if (rc == H2_PAL_OK &&
      h2_pal_wifi_settings_validate_sta_config(&saved) != H2_PAL_OK) {
    memset(&saved, 0, sizeof(saved));
    return H2_PAL_ERR_FORMAT;
  }
  const uint8_t salt[] = "iperf-wifi-preservation-v1";
  uint8_t record[108] = {0};
  record[0] = rc == H2_PAL_OK;
  record[1] = (uint8_t)saved.ssid_len;
  record[2] = (uint8_t)saved.password_len;
  record[3] = saved.bssid_set;
  record[4] = saved.channel;
  memcpy(record + 5, saved.bssid, 6u);
  memcpy(record + 11, saved.ssid, saved.ssid_len);
  memcpy(record + 43, saved.password, saved.password_len);
  record[107] = 1u;
  rc = h2_pal_crypto_hkdf_sha256(runtime->crypto, record, sizeof(record), salt,
                                 sizeof(salt) - 1u, NULL, 0u, out, 32u);
  memset(record, 0, sizeof(record));
  memset(&saved, 0, sizeof(saved));
  return rc;
}

static void report_network(h2_runtime_t *runtime, const char *target,
                           const char *phase,
                           const h2_iperf_client_app_network_t *network) {
  const h2_pal_wifi_sta_status_t *wifi = &network->wifi;
  const uint8_t *ip = wifi->ip.ip6;
  char line[400];
  (void)snprintf(line, sizeof(line),
                 "H2_IPERF_CLIENT_LINK board=%s phase=%s mode=%u pal_state=%u "
                 "ipv4_valid=%u ipv4=%lu ipv6_ready=%u runtime_ready=%u "
                 "ipv6=%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                 "%02x%02x:%02x%02x rssi=%d channel=%u "
                 "bssid=%02x%02x%02x%02x%02x%02x power_save=0 ble_service=0",
                 target, phase, (unsigned)network->mode, (unsigned)wifi->state,
                 wifi->ip_valid, (unsigned long)wifi->ip.ip4,
                 wifi->ip.ip6_valid, network->runtime_ready, ip[0], ip[1],
                 ip[2], ip[3], ip[4], ip[5], ip[6], ip[7], ip[8], ip[9], ip[10],
                 ip[11], ip[12], ip[13], ip[14], ip[15], wifi->rssi,
                 wifi->channel, wifi->bssid[0], wifi->bssid[1], wifi->bssid[2],
                 wifi->bssid[3], wifi->bssid[4], wifi->bssid[5]);
  (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "iperf-client", line);
}

static int verify_disconnected(h2_runtime_t *runtime) {
  uint64_t started = 0u;
  int rc = h2_pal_time_get_monotonic_ms(runtime->time, &started);
  if (rc != H2_PAL_OK)
    return rc;
  int lost = 0;
  for (;;) {
    union {
      h2_runtime_system_event_wifi_sta_t wifi;
      uint8_t bytes[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    } payload;
    h2_runtime_event_t event = {.payload = payload.bytes,
                                .payload_capacity = sizeof(payload.bytes)};
    while ((rc = h2_runtime_poll_event(runtime, &event)) == H2_PAL_OK)
      lost |= event.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_LOST_IP;
    if (rc != H2_PAL_ERR_WOULD_BLOCK && rc != H2_PAL_ERR_TIMEOUT)
      return rc;
    h2_pal_wifi_sta_status_t wifi = {0};
    rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &wifi);
    if (rc != H2_PAL_OK)
      return rc;
    h2_runtime_system_wifi_sta_state_t state = {0};
    rc = h2_runtime_system_state_wifi_sta(runtime, &state);
    h2_pal_netif_ref_t ref = {.type = H2_PAL_NETIF_REF_KIND,
                              .kind = H2_PAL_NETIF_KIND_WIFI_STA};
    h2_pal_netif_status_t netif = {0};
    int netif_rc = h2_pal_netif_get_status(runtime->netif, &ref, &netif);
    if (rc == H2_PAL_OK && lost && !wifi.ip_valid && !wifi.ip.ip6_valid &&
        !state.ip_valid && !state.ip.ip6_valid &&
        (netif_rc == H2_PAL_ERR_NOT_FOUND ||
         (netif_rc == H2_PAL_OK &&
          !(netif.flags &
            (H2_PAL_NETIF_FLAG_HAS_IPV4 | H2_PAL_NETIF_FLAG_HAS_IPV6)))))
      return H2_PAL_OK;
    uint64_t now = 0u;
    if (h2_pal_time_get_monotonic_ms(runtime->time, &now) != H2_PAL_OK ||
        now < started || now - started >= 5000u)
      return H2_PAL_ERR_TIMEOUT;
    rc = h2_pal_time_sleep_ms(runtime->time, 100u);
    if (rc != H2_PAL_OK)
      return rc;
  }
}

h2_pal_result_t h2_iperf_client_app_bench(h2_runtime_t *runtime,
                                          const char *target,
                                          h2_iperf_e2e_checkpoint_fn checkpoint,
                                          void *checkpoint_user) {
  if (runtime == NULL || target == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  uint8_t before[32] = {0}, after[32] = {0};
  h2_pal_wifi_sta_config_t wifi = {.ssid = "GizOS-iPerf",
                                   .ssid_len = 11u,
                                   .password = "gizosiperf",
                                   .password_len = 10u,
                                   .channel = 6u};
  h2_iperf_client_app_network_t network = {0};
  h2_iperf_client_app_config_t config = {
      .target = target,
      .ipv4 = {.family = H2_PAL_NET_FAMILY_IPV4, .ip = {192, 168, 4, 1}},
      .ipv6 = {.family = H2_PAL_NET_FAMILY_IPV6,
               .ip = {0xfd, 0x53, 0x69, 0x7a, 0x6f, 0x73, 0x06, 0x26, 0, 0, 0,
                      0, 0, 0, 0, 1}},
      .rounds = 3u,
      .duration_ms = 5000u,
      .checkpoint = checkpoint,
      .checkpoint_user = checkpoint_user};
  h2_iperf_client_app_report_t report = {0};
  int matrix = H2_PAL_ERR_INVALID_STATE;
  int matrix_started = 0, radio_attempted = 0;
  int initial_ready = 0, reconnected = 0;
  int rc = saved_signature(runtime, before);
  const int before_valid = rc == H2_PAL_OK;
  if (rc != H2_PAL_OK)
    goto finished;
  radio_attempted = 1;
  rc = h2_iperf_client_app_connect(runtime, &wifi, &network);
  if (rc != H2_PAL_OK)
    goto finished;
  config.mode = network.mode;
  initial_ready = network.runtime_ready;
  report_network(runtime, target, "initial", &network);
  matrix_started = 1;
  matrix = h2_iperf_client_app_run(runtime, &config, &report);
  rc = matrix;
  int lifecycle = h2_pal_wifi_sta_disconnect(runtime->wifi_sta);
  if (lifecycle == H2_PAL_OK)
    lifecycle = verify_disconnected(runtime);
  if (lifecycle == H2_PAL_OK)
    lifecycle = h2_iperf_client_app_connect(runtime, &wifi, &network);
  if (lifecycle == H2_PAL_OK && network.mode != config.mode)
    lifecycle = H2_PAL_ERR_INVALID_STATE;
  if (lifecycle == H2_PAL_OK) {
    reconnected = 1;
    report_network(runtime, target, "reconnect", &network);
  }
  if (rc == H2_PAL_OK)
    rc = lifecycle;
finished:
  if (rc != H2_PAL_OK && radio_attempted)
    (void)h2_pal_wifi_sta_disconnect(runtime->wifi_sta);
  /* Every valid bench reaches the final settings observation, even if setup
   * never started the matrix. Keep the first qualification error intact. */
  int saved_check = saved_signature(runtime, after);
  if (saved_check == H2_PAL_OK &&
      (!before_valid || memcmp(before, after, sizeof(before))))
    saved_check = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK && saved_check != H2_PAL_OK) {
    rc = saved_check;
    if (radio_attempted)
      (void)h2_pal_wifi_sta_disconnect(runtime->wifi_sta);
  }
  const int qualified = rc == H2_PAL_OK;
  char line[384];
  (void)snprintf(
      line, sizeof(line),
      "H2_IPERF_CLIENT_COMPLETE board=%s mode=%u rc=%d passed=%u total=%u "
      "matrix_rc=%d public_wifi=%u runtime_ip=%u disconnect_reconnect=%u "
      "saved_unchanged=%u matrix_started=%u saved_check_rc=%d",
      target, (unsigned)config.mode, rc, report.passed, report.total, matrix,
      (unsigned)(qualified && initial_ready),
      (unsigned)(qualified && initial_ready),
      (unsigned)(qualified && reconnected),
      (unsigned)(qualified && saved_check == H2_PAL_OK),
      (unsigned)matrix_started, saved_check);
  (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "iperf-client", line);
  memset(&wifi, 0, sizeof(wifi));
  memset(before, 0, sizeof(before));
  memset(after, 0, sizeof(after));
  return rc;
}

h2_pal_result_t
h2_iperf_client_app_connect(h2_runtime_t *runtime,
                            const h2_pal_wifi_sta_config_t *config,
                            h2_iperf_client_app_network_t *out_network) {
  if (out_network != NULL)
    memset(out_network, 0, sizeof(*out_network));
  if (runtime == NULL || config == NULL || out_network == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (h2_pal_wifi_settings_validate_sta_config(config) != H2_PAL_OK)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_pal_wifi_sta_connect(runtime->wifi_sta, config, 20000u);
  if (rc != H2_PAL_OK)
    goto failed;
  rc = h2_pal_wifi_sta_set_power_save(runtime->wifi_sta,
                                      H2_PAL_WIFI_POWER_SAVE_NONE);
  if (rc != H2_PAL_OK)
    goto failed;
  uint64_t started = 0u;
  rc = h2_pal_time_get_monotonic_ms(runtime->time, &started);
  if (rc != H2_PAL_OK)
    goto failed;
  int observed_ready = 0;
  for (;;) {
    union {
      h2_runtime_system_event_wifi_sta_t wifi;
      uint8_t bytes[H2_RUNTIME_EVENT_PAYLOAD_MAX];
    } payload;
    h2_runtime_event_t event = {.payload = payload.bytes,
                                .payload_capacity = sizeof(payload.bytes)};
    while ((rc = h2_runtime_poll_event(runtime, &event)) == H2_PAL_OK) {
      if (event.kind == H2_RUNTIME_SYSTEM_EVENT_WIFI_STA_GOT_IP &&
          event.payload_size == sizeof(h2_runtime_system_event_wifi_sta_t)) {
        const h2_runtime_system_event_wifi_sta_t *ip = event.payload;
        if (ip->ssid_len == config->ssid_len &&
            memcmp(ip->ssid, config->ssid, config->ssid_len) == 0 &&
            ((ip->ip_valid && ip->ip.ip4) || ip->ip.ip6_valid))
          observed_ready = 1;
      }
    }
    if (rc != H2_PAL_ERR_WOULD_BLOCK && rc != H2_PAL_ERR_TIMEOUT)
      goto failed;
    h2_pal_wifi_sta_status_t wifi = {0};
    rc = h2_pal_wifi_sta_get_status(runtime->wifi_sta, &wifi);
    if (rc != H2_PAL_OK)
      goto failed;
    uint64_t now = 0u;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (rc != H2_PAL_OK)
      goto failed;
    if (now < started || now - started >= 45000u) {
      rc = H2_PAL_ERR_TIMEOUT;
      goto failed;
    }
    if (now - started >= 15000u && h2_pal_wifi_sta_status_has_ip(&wifi) &&
        wifi.ssid_len == config->ssid_len &&
        memcmp(wifi.ssid, config->ssid, config->ssid_len) == 0) {
      h2_runtime_system_wifi_sta_state_t state = {0};
      rc = h2_runtime_system_state_wifi_sta(runtime, &state);
      if (rc == H2_PAL_OK && observed_ready && same_addresses(&wifi, &state)) {
        h2_pal_net_addr_t local = {0};
        if (wifi.ip_valid) {
          rc = h2_pal_net_get_host_addr_family(runtime->net, NULL,
                                               H2_PAL_NET_FAMILY_IPV4, &local);
          uint8_t expected[4];
          h2_pal_wifi_ip4_to_bytes(wifi.ip.ip4, expected);
          if (rc != H2_PAL_OK || memcmp(local.ip, expected, sizeof(expected))) {
            rc = H2_PAL_ERR_INVALID_STATE;
            goto failed;
          }
        }
        if (wifi.ip.ip6_valid) {
          rc = h2_pal_net_get_host_addr_family(runtime->net, NULL,
                                               H2_PAL_NET_FAMILY_IPV6, &local);
          if (rc != H2_PAL_OK || memcmp(local.ip, wifi.ip.ip6, 16u)) {
            rc = H2_PAL_ERR_INVALID_STATE;
            goto failed;
          }
        }
        out_network->mode = wifi.ip_valid && wifi.ip.ip6_valid
                                ? H2_IPERF_CLIENT_APP_DUAL
                            : wifi.ip_valid ? H2_IPERF_CLIENT_APP_IPV4
                                            : H2_IPERF_CLIENT_APP_IPV6;
        out_network->wifi = wifi;
        out_network->runtime_ready = 1u;
        return H2_PAL_OK;
      }
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_NOT_FOUND)
        goto failed;
    }
    rc = h2_pal_time_sleep_ms(runtime->time, 100u);
    if (rc != H2_PAL_OK)
      goto failed;
  }
failed:
  /* Preserve the setup error if the provider also fails to disconnect. */
  (void)h2_pal_wifi_sta_disconnect(runtime->wifi_sta);
  return rc;
}
