#include "h2_pal_ipv6_e2e.h"
#include "client.h"
#include "h2_corehttp.h"
#include "h2_coremqtt.h"
#include "h2_dns.h"
#include "h2_pal_webrtc_e2e.h"
#include <stdio.h>
#include <string.h>

static const char *ids[H2_PAL_IPV6_EXTRA_CASES] = {
    "dns-ipv4-filter",           "dns-ipv6-filter",
    "dns-any-families",          "dns-family-async-copy",
    "dns-family-cancel",         "invalid-family",
    "link-local-scope-required", "ipv6-socket-isolation",
    "netif-ipv6-address",        "http-ipv6-literal",
    "http-dual-stack-fallback",  "mqtt-ipv6",
    "dtls-ipv6-payload",         "dtls-ipv6-fingerprint-rejected",
    "dtls-ipv6-deadline",        "webrtc-ipv6-matrix",
    "netif-ipv6-udp-bind",       "netif-ipv6-tcp-bind",
    "dns-aaaa-ipv6-wire"};

static int contains(const h2_pal_net_addr_list_t *list,
                    h2_pal_net_family_t family) {
  if (!list->count || list->count > H2_PAL_NET_ADDR_MAX)
    return 0;
  for (size_t i = 0u; i < list->count; ++i)
    if (list->addrs[i].family == family)
      return 1;
  return 0;
}
static int resolve(const h2_pal_net_api_t *net, const char *host,
                   h2_pal_net_family_t family) {
  h2_pal_net_addr_list_t list;
  int rc = h2_pal_net_resolve_all(net, host, family, &list);
  if (rc != H2_PAL_OK)
    return rc;
  if (!contains(&list, family))
    return H2_PAL_ERR_FORMAT;
  for (size_t i = 0u; i < list.count; ++i)
    if (list.addrs[i].family != family)
      return H2_PAL_ERR_FORMAT;
  return H2_PAL_OK;
}
static int http(const h2_pal_ipv6_config_t *config, const char *url) {
  if (!url || !url[0])
    return H2_PAL_ERR_UNAVAILABLE;
  const h2_runtime_t *runtime = config->transport.runtime;
  h2_corehttp_config_t settings = {
      .allocator = runtime->mem,
      .net = runtime->net,
      .time = runtime->time,
      .log = runtime->log,
      .default_timeout_ms = 5000u,
      .root_ca_pem = config->transport.root_ca,
      .root_ca_pem_len = config->transport.root_ca_len,
      .tls_verify = H2_PAL_NET_TLS_VERIFY_REQUIRED};
  h2_corehttp_t *provider = NULL;
  h2_pal_http_api_t api;
  int rc = h2_corehttp_create(&settings, &provider, &api);
  if (rc != H2_PAL_OK)
    return rc;
  uint8_t body[64];
  h2_pal_http_request_t request = {.url = {.data = url, .len = strlen(url)},
                                   .method = H2_PAL_HTTP_GET,
                                   .timeout_ms = 5000,
                                   .response_buf = body,
                                   .response_buf_cap = sizeof(body)};
  h2_pal_http_response_t response;
  memset(&response, 0, sizeof(response));
  rc = h2_pal_http_request(&api, &request, &response);
  if (rc == H2_PAL_OK &&
      (response.status_code != 200 ||
       response.body_len != strlen(config->transport.session) ||
       memcmp(body, config->transport.session, response.body_len) != 0))
    rc = H2_PAL_ERR_FORMAT;
  h2_pal_http_response_free(&api, &response);
  h2_corehttp_destroy(provider);
  return rc;
}
typedef struct local_http_peer {
  const h2_runtime_t *runtime;
  int listener;
  int rc;
  const char *session;
} local_http_peer_t;
static void local_http_serve(void *user) {
  local_http_peer_t *peer = user;
  int client = -1;
  h2_pal_net_addr_t remote;
  const h2_pal_net_api_t *net = peer->runtime->net;
  peer->rc =
      h2_pal_net_tcp_accept(net, peer->listener, &client, &remote, 5000u);
  if (peer->rc == H2_PAL_OK && remote.family != H2_PAL_NET_FAMILY_IPV4)
    peer->rc = H2_PAL_ERR_FORMAT;
  char request[1024] = {0};
  size_t used = 0u;
  while (peer->rc == H2_PAL_OK && strstr(request, "\r\n\r\n") == NULL) {
    int got = h2_pal_net_tcp_recv(net, client, (uint8_t *)request + used,
                                  sizeof(request) - used - 1u, 1000u);
    if (got <= 0 || (size_t)got >= sizeof(request) - used) {
      peer->rc = got < 0 ? got : H2_PAL_ERR_FORMAT;
      break;
    }
    used += (size_t)got;
    if (used == sizeof(request) - 1u)
      peer->rc = H2_PAL_ERR_NO_SPACE;
  }
  if (peer->rc == H2_PAL_OK && strstr(request, peer->session) == NULL)
    peer->rc = H2_PAL_ERR_FORMAT;
  const char headers[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 32\r\nConnection: close\r\n\r\n";
  char reply[sizeof(headers) + 32u];
  memcpy(reply, headers, sizeof(headers) - 1u);
  memcpy(reply + sizeof(headers) - 1u, peer->session, 32u);
  size_t offset = 0u, size = sizeof(headers) - 1u + 32u;
  while (peer->rc == H2_PAL_OK && offset < size) {
    int sent = h2_pal_net_tcp_send_timeout(
        net, client, (const uint8_t *)reply + offset, size - offset, 1000u);
    if (sent <= 0) {
      peer->rc = sent < 0 ? sent : H2_PAL_ERR_IO;
      break;
    }
    offset += (size_t)sent;
  }
  h2_pal_net_close(net, client);
}
static int http_local_fallback(const h2_pal_ipv6_config_t *config) {
  const h2_runtime_t *runtime = config->transport.runtime;
  h2_pal_net_addr_t address;
  h2_pal_net_bind_t bind = {
      .type = H2_PAL_NET_BIND_SOURCE_ADDR,
      .source_addr = {.family = H2_PAL_NET_FAMILY_IPV4, .ip = {127, 0, 0, 1}}};
  local_http_peer_t peer = {
      .runtime = runtime, .listener = -1, .session = config->transport.session};
  int rc = h2_pal_net_tcp_listen(runtime->net, H2_PAL_NET_FAMILY_IPV4, 0u,
                                 &bind, &peer.listener, &address);
  if (rc != H2_PAL_OK)
    return rc;
  h2_pal_task_t *task = NULL;
  h2_pal_task_options_t options = {.name = "pal-ipv6/e2e/http-peer"};
  rc = h2_pal_task_start(runtime->task, &options, local_http_serve, &peer,
                         &task);
  if (rc == H2_PAL_OK) {
    char url[128];
    snprintf(url, sizeof(url), "http://localhost:%u/%s", (unsigned)address.port,
             config->transport.session);
    rc = http(config, url);
    int joined = h2_pal_task_join(runtime->task, task);
    if (rc == H2_PAL_OK)
      rc = joined != H2_PAL_OK ? joined : peer.rc;
  }
  h2_pal_net_close(runtime->net, peer.listener);
  return rc;
}
static int mqtt(const h2_pal_ipv6_config_t *config) {
  if (!config->mqtt_host || !config->mqtt_port)
    return H2_PAL_ERR_UNAVAILABLE;
  const h2_runtime_t *runtime = config->transport.runtime;
  h2_coremqtt_config_t settings = {.allocator = runtime->mem,
                                   .net = runtime->net,
                                   .time = runtime->time,
                                   .log = runtime->log};
  h2_coremqtt_t *provider = NULL;
  h2_pal_mqtt_api_t api;
  int rc = h2_coremqtt_create(&settings, &provider, &api);
  if (rc != H2_PAL_OK)
    return rc;
  uint8_t buffer[2048];
  h2_pal_mqtt_client_config_t client_settings = {
      .endpoint = {.host = {.data = config->mqtt_host,
                            .len = strlen(config->mqtt_host)},
                   .port = config->mqtt_port},
      .client_id = {.data = config->transport.session,
                    .len = strlen(config->transport.session)},
      .network_buffer = buffer,
      .network_buffer_len = sizeof(buffer),
      .connect_timeout_ms = 5000u,
      .operation_timeout_ms = 1000u,
      .keepalive_sec = 10u,
      .clean_session = 1};
  h2_pal_mqtt_client_t *client = NULL;
  rc = h2_pal_mqtt_open(&api, &client_settings, &client);
  if (rc == H2_PAL_OK)
    rc = h2_pal_mqtt_connect(&api, client);
  if (rc == H2_PAL_OK) {
    h2_pal_mqtt_publish_t message = {
        .topic = {.data = "pal/ipv6", .len = 8u},
        .payload = {.data = (const uint8_t *)config->transport.session,
                    .len = strlen(config->transport.session)},
        .timeout_ms = 1000u};
    rc = h2_pal_mqtt_publish(&api, client, &message, NULL);
    if (rc == H2_PAL_OK)
      rc = h2_pal_mqtt_disconnect(&api, client, 1000u);
  }
  if (client)
    h2_pal_mqtt_close(&api, client);
  h2_coremqtt_destroy(provider);
  return rc;
}
typedef struct webrtc_fixture {
  h2_webrtc_fixture_client_t client;
  unsigned ipv6_pairs;
  const h2_pal_log_api_t *log;
} webrtc_fixture_t;
static int webrtc_close(void *user) {
  webrtc_fixture_t *fixture = user;
  const h2_webrtc_fixture_client_t *client = &fixture->client;
  size_t length = strlen(client->offer_url);
  char url[512], body[1024] = {0};
  int size = snprintf(url, sizeof(url), "%.*s/session/%s/ice-pair",
                      (int)(length - 6u), client->offer_url, client->session);
  if (length < 6u || size <= 0 || (size_t)size >= sizeof(url))
    return H2_PAL_ERR_FORMAT;
  h2_pal_http_request_t request = {.url = {url, (size_t)size},
                                   .method = H2_PAL_HTTP_GET,
                                   .response_buf = (uint8_t *)body,
                                   .response_buf_cap = sizeof(body) - 1u,
                                   .timeout_ms = 1000};
  h2_pal_http_response_t response = {0};
  int rc = h2_pal_http_do(client->http, &request, &response);
  if (rc == H2_PAL_OK && response.status_code == 200 &&
      strstr(body, "\"local_family\":6") && strstr(body, "\"remote_family\":6"))
    ++fixture->ipv6_pairs;
  char line[120];
  snprintf(line, sizeof(line),
           "H2_PAL_IPV6_ICE_WITNESS status=%d rc=%d local6=%d remote6=%d",
           response.status_code, rc, strstr(body, "\"local_family\":6") != NULL,
           strstr(body, "\"remote_family\":6") != NULL);
  h2_pal_log_write(fixture->log, H2_PAL_LOG_INFO, "pal-ipv6", line);
  h2_pal_http_response_free(client->http, &response);
  return h2_webrtc_fixture_close(&fixture->client);
}
static void webrtc_report(void *user,
                          const h2_pal_webrtc_e2e_case_result_t *item) {
  const h2_runtime_t *runtime = user;
  char line[256];
  snprintf(line, sizeof(line),
           "H2_PAL_IPV6_WEBRTC_CASE id=%s passed=%d blocked=%d detail=%d "
           "line=%u elapsed_ms=%llu",
           item->id, item->passed, item->blocked, item->detail, item->line,
           (unsigned long long)item->elapsed_ms);
  h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-ipv6", line);
}
static int webrtc(const h2_pal_ipv6_config_t *config) {
  if (!config->offer_url || !config->stun_url)
    return H2_PAL_ERR_UNAVAILABLE;
  const h2_runtime_t *runtime = config->transport.runtime;
  h2_corehttp_config_t settings = {.allocator = runtime->mem,
                                   .net = runtime->net,
                                   .time = runtime->time,
                                   .log = runtime->log,
                                   .default_timeout_ms = 10000u};
  h2_corehttp_t *http = NULL;
  h2_pal_http_api_t api;
  int rc = h2_corehttp_create(&settings, &http, &api);
  if (rc != H2_PAL_OK)
    return rc;
  webrtc_fixture_t witness = {
      .client = {.http = &api, .offer_url = config->offer_url}, .log = runtime->log};
  h2_pal_webrtc_e2e_config_t fixture = {.runtime = runtime,
                                        .stun_url = config->stun_url,
                                        .exchange_offer =
                                            h2_webrtc_fixture_exchange,
                                        .close_remote = webrtc_close,
                                        .fixture_user = &witness,
                                        .report = webrtc_report,
                                        .report_user = (void *)runtime,
                                        .connection_timeout_ms = 10000u};
  h2_pal_webrtc_e2e_result_t result;
  rc = h2_pal_webrtc_e2e_run(&fixture, &result);
  if (witness.client.session[0]) {
    int closed = webrtc_close(&witness);
    if (rc == H2_PAL_OK)
      rc = closed;
  }
  if (rc == H2_PAL_OK && witness.ipv6_pairs == 0u)
    rc = H2_PAL_ERR_FORMAT;
  h2_corehttp_destroy(http);
  return rc;
}
typedef struct dtls_endpoint {
  const h2_pal_net_api_t *net;
  int socket;
  h2_pal_net_addr_t peer;
  uint8_t plaintext[64];
  size_t plaintext_len;
  unsigned sent, received;
} dtls_endpoint_t;
static int dtls_send(void *user, const uint8_t *data, size_t size) {
  dtls_endpoint_t *endpoint = user;
  int sent = h2_pal_net_udp_sendto(endpoint->net, endpoint->socket,
                                   &endpoint->peer, data, size);
  if (sent == (int)size)
    ++endpoint->sent;
  return sent == (int)size ? H2_PAL_OK : sent < 0 ? sent : H2_PAL_ERR_IO;
}
static int dtls_plaintext(void *user, const uint8_t *data, size_t size) {
  dtls_endpoint_t *endpoint = user;
  if (size > sizeof(endpoint->plaintext))
    return H2_PAL_ERR_NO_SPACE;
  memcpy(endpoint->plaintext, data, size);
  endpoint->plaintext_len = size;
  return H2_PAL_OK;
}
static int dtls(const h2_pal_ipv6_config_t *config, unsigned mode) {
  const h2_runtime_t *runtime = config->transport.runtime;
  const h2_pal_dtls_api_t *api = config->dtls;
  dtls_endpoint_t endpoints[2] = {{.net = runtime->net, .socket = -1},
                                  {.net = runtime->net, .socket = -1}};
  h2_pal_dtls_session_t *sessions[2] = {NULL, NULL};
  h2_pal_net_addr_t addresses[2];
  uint8_t fingerprints[2][H2_PAL_DTLS_SHA256_FINGERPRINT_SIZE];
  int complete[2] = {0, 0};
  int rc = H2_PAL_OK;
  uint64_t now = 0u;
  for (unsigned i = 0u; i < 2u; ++i) {
    rc = h2_pal_net_udp_open_bound(runtime->net, H2_PAL_NET_FAMILY_IPV6, 0u,
                                   NULL, &endpoints[i].socket, &addresses[i]);
    if (rc != H2_PAL_OK)
      goto cleanup;
    memset(addresses[i].ip, 0, 16u);
    addresses[i].ip[15] = 1u;
    h2_pal_dtls_session_config_t settings = {
        .role = i ? H2_PAL_DTLS_ROLE_SERVER : H2_PAL_DTLS_ROLE_CLIENT,
        .max_datagram_size = 1500u,
        .max_plaintext_size = 512u,
        .max_pending_output_bytes = 32768u,
        .send = dtls_send,
        .plaintext = dtls_plaintext,
        .io_user = &endpoints[i]};
    rc = h2_pal_dtls_session_create(api, &settings, &sessions[i]);
    if (rc != H2_PAL_OK)
      goto cleanup;
    rc = h2_pal_dtls_session_get_local_fingerprint(api, sessions[i],
                                                   fingerprints[i]);
    if (rc != H2_PAL_OK)
      goto cleanup;
  }
  for (unsigned i = 0u; i < 2u; ++i) {
    endpoints[i].peer = addresses[1u - i];
    if (mode == 1u && i == 0u)
      fingerprints[1][0] ^= 1u;
    rc = h2_pal_dtls_session_set_remote_fingerprint(api, sessions[i],
                                                    fingerprints[1u - i]);
    if (rc != H2_PAL_OK)
      goto cleanup;
  }
  rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
  if (rc != H2_PAL_OK)
    goto cleanup;
  uint64_t deadline = now + 5000u;
  if (mode == 2u) {
    deadline = now + 100u;
    rc = h2_pal_dtls_session_handshake(api, sessions[0], NULL, 0u, now,
                                       deadline, &complete[0]);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK)
      goto cleanup;
    rc = h2_pal_time_sleep_ms(runtime->time, 110u);
    if (rc == H2_PAL_OK)
      rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (rc != H2_PAL_OK)
      goto cleanup;
    rc = h2_pal_dtls_session_handshake(api, sessions[0], NULL, 0u, now,
                                       deadline, &complete[0]);
    rc = rc == H2_PAL_ERR_TIMEOUT ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
    goto cleanup;
  }
  rc = h2_pal_dtls_session_handshake(api, sessions[0], NULL, 0u, now, deadline,
                                     &complete[0]);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK)
    goto verdict;
  while (!complete[0] || !complete[1]) {
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
    if (rc != H2_PAL_OK || now >= deadline) {
      rc = rc != H2_PAL_OK ? rc : H2_PAL_ERR_TIMEOUT;
      goto verdict;
    }
    for (unsigned i = 0u; i < 2u; ++i) {
      uint8_t packet[1500];
      h2_pal_net_addr_t source;
      int size = h2_pal_net_udp_recvfrom(runtime->net, endpoints[i].socket,
                                         &source, packet, sizeof(packet), 5u);
      if (size > 0)
        ++endpoints[i].received;
      if (size < 0 && size != H2_PAL_ERR_WOULD_BLOCK &&
          size != H2_PAL_ERR_TIMEOUT) {
        rc = size;
        goto verdict;
      }
      int previous_complete = complete[i];
      if (complete[i]) {
        /* A completed DTLS endpoint still services duplicate handshake
         * flights through its datagram consumer until the peer completes. */
        rc = size > 0 ? h2_pal_dtls_session_consume_datagram(
                            api, sessions[i], packet, (size_t)size)
                      : H2_PAL_OK;
      } else {
        rc = h2_pal_dtls_session_handshake(
            api, sessions[i], size > 0 ? packet : NULL,
            size > 0 ? (size_t)size : 0u, now, deadline, &complete[i]);
      }
      if (size > 0 || complete[i] != previous_complete ||
          (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK)) {
        char line[180];
        snprintf(line, sizeof(line),
                 "H2_PAL_IPV6_DTLS_STEP mode=%u side=%u bytes=%d rc=%d "
                 "complete=%d tx=%u rx=%u",
                 mode, i, size, rc, complete[i], endpoints[i].sent,
                 endpoints[i].received);
        h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-ipv6", line);
      }
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK)
        goto verdict;
    }
  }
  if (mode == 1u) {
    rc = H2_PAL_ERR_FORMAT;
    goto cleanup;
  }
  rc = h2_pal_dtls_session_write(api, sessions[0],
                                 (const uint8_t *)config->transport.session,
                                 strlen(config->transport.session));
  if (rc == H2_PAL_OK) {
    uint8_t packet[1500];
    h2_pal_net_addr_t source;
    int size = h2_pal_net_udp_recvfrom(runtime->net, endpoints[1].socket,
                                       &source, packet, sizeof(packet), 1000u);
    rc = size > 0 ? h2_pal_dtls_session_consume_datagram(api, sessions[1],
                                                         packet, (size_t)size)
                  : size;
    if (rc == H2_PAL_OK &&
        (endpoints[1].plaintext_len != strlen(config->transport.session) ||
         memcmp(endpoints[1].plaintext, config->transport.session,
                endpoints[1].plaintext_len) != 0))
      rc = H2_PAL_ERR_FORMAT;
  }
  goto cleanup;
verdict: {
  char line[180];
  snprintf(line, sizeof(line),
           "H2_PAL_IPV6_DTLS_VERDICT mode=%u rc=%d c0=%d c1=%d tx0=%u tx1=%u "
           "rx0=%u rx1=%u",
           mode, rc, complete[0], complete[1], endpoints[0].sent,
           endpoints[1].sent, endpoints[0].received, endpoints[1].received);
  h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "pal-ipv6", line);
}
  if (mode == 1u)
    rc = rc == H2_PAL_ERR_TLS_VERIFY && !endpoints[0].plaintext_len &&
                 !endpoints[1].plaintext_len
             ? H2_PAL_OK
             : H2_PAL_ERR_FORMAT;
cleanup:
  for (unsigned i = 0u; i < 2u; ++i) {
    if (sessions[i])
      h2_pal_dtls_session_destroy(api, &sessions[i]);
    h2_pal_net_close(runtime->net, endpoints[i].socket);
  }
  return rc;
}

static int bound_interface(const h2_pal_ipv6_config_t *config, int tcp) {
  const h2_pal_net_api_t *net = config->transport.runtime->net;
  h2_pal_net_bind_t bind = {.type = H2_PAL_NET_BIND_NETIF,
                            .netif = &config->netif};
  h2_pal_net_addr_t target = config->local_ipv6;
  if (target.family != H2_PAL_NET_FAMILY_IPV6) {
    int found = h2_pal_net_get_host_addr_family(
        net, NULL, H2_PAL_NET_FAMILY_IPV6, &target);
    if (found != H2_PAL_OK)
      return found;
  }
  int sockets[3] = {-1, -1, -1};
  h2_pal_net_addr_t bound = {0}, peer = {0};
  int rc;
  if (tcp) {
    rc = h2_pal_net_tcp_listen(net, target.family, 0u, &bind, &sockets[0],
                               &bound);
    if (rc != H2_PAL_OK)
      goto cleanup;
    target.port = bound.port;
    rc = h2_pal_net_tcp_open_bound(net, target.family, &bind, &sockets[1]);
    if (rc == H2_PAL_OK)
      rc = h2_pal_net_tcp_connect(net, sockets[1], &target, 1000u);
    if (rc == H2_PAL_OK)
      rc = h2_pal_net_tcp_accept(net, sockets[0], &sockets[2], &peer, 1000u);
    if (rc == H2_PAL_OK) {
      int sent = h2_pal_net_tcp_send_timeout(
          net, sockets[1], (const uint8_t *)"ipv6", 4u, 1000u);
      uint8_t bytes[4];
      int got = sent == 4 ? h2_pal_net_tcp_recv(net, sockets[2], bytes,
                                                sizeof(bytes), 1000u)
                          : sent;
      rc = got == 4 && memcmp(bytes, "ipv6", 4u) == 0 ? H2_PAL_OK
           : got < 0                                  ? got
                                                      : H2_PAL_ERR_FORMAT;
    }
  } else {
    rc = h2_pal_net_udp_open_bound(net, target.family, 0u, &bind, &sockets[0],
                                   &bound);
    if (rc != H2_PAL_OK)
      goto cleanup;
    target.port = bound.port;
    int sent = h2_pal_net_udp_sendto(net, sockets[0], &target,
                                     (const uint8_t *)"ipv6", 4u);
    uint8_t bytes[4];
    int got = sent == 4 ? h2_pal_net_udp_recvfrom(net, sockets[0], &peer, bytes,
                                                  sizeof(bytes), 1000u)
                        : sent;
    rc = got == 4 && peer.family == target.family &&
                 peer.scope_id == target.scope_id &&
                 memcmp(peer.ip, target.ip, 16u) == 0 &&
                 memcmp(bytes, "ipv6", 4u) == 0
             ? H2_PAL_OK
         : got < 0 ? got
                   : H2_PAL_ERR_FORMAT;
  }
cleanup:
  for (unsigned i = 0u; i < 3u; ++i)
    h2_pal_net_close(net, sockets[i]);
  return rc;
}
static int extra(const h2_pal_ipv6_config_t *config, unsigned index) {
  const h2_runtime_t *runtime = config->transport.runtime;
  const h2_pal_net_api_t *net = runtime->net;
  h2_pal_net_addr_list_t list = {0};
  int rc = H2_PAL_OK;
  switch (index) {
  case 0:
    return resolve(net, "127.0.0.1", H2_PAL_NET_FAMILY_IPV4);
  case 1:
    return resolve(net, "::1", H2_PAL_NET_FAMILY_IPV6);
  case 2:
    rc = h2_pal_net_resolve_all(
        net, config->dual_dns_host ? config->dual_dns_host : "localhost",
        H2_PAL_NET_FAMILY_ANY, &list);
    return rc != H2_PAL_OK ? rc
           : contains(&list, H2_PAL_NET_FAMILY_IPV4) &&
                   contains(&list, H2_PAL_NET_FAMILY_IPV6)
               ? H2_PAL_OK
               : H2_PAL_ERR_FORMAT;
  case 3: {
    char host[] = "::1";
    h2_pal_net_resolver_t *resolver = NULL;
    rc = h2_pal_net_resolve_start_family(net, host, H2_PAL_NET_FAMILY_IPV6,
                                         &resolver);
    memset(host, 'x', sizeof(host) - 1u);
    if (rc == H2_PAL_OK) {
      rc = h2_pal_net_resolve_poll_all(net, resolver, &list, 5000u);
      if (rc == H2_PAL_OK && !contains(&list, H2_PAL_NET_FAMILY_IPV6))
        rc = H2_PAL_ERR_FORMAT;
    }
    h2_pal_net_resolve_close(net, resolver);
    return rc;
  }
  case 4: {
    uint64_t start = 0u;
    rc = h2_pal_time_get_monotonic_ms(runtime->time, &start);
    if (rc != H2_PAL_OK)
      return rc;
    for (unsigned i = 0u; i < 32u; ++i) {
      h2_pal_net_resolver_t *resolver = NULL;
      rc = h2_pal_net_resolve_start_family(net, "::1", H2_PAL_NET_FAMILY_IPV6,
                                           &resolver);
      if (rc == H2_PAL_ERR_NO_SPACE) {
        uint64_t now = 0u;
        rc = h2_pal_time_get_monotonic_ms(runtime->time, &now);
        if (rc != H2_PAL_OK || now - start >= 5000u)
          return rc != H2_PAL_OK ? rc : H2_PAL_ERR_TIMEOUT;
        h2_pal_time_sleep_ms(runtime->time, 10u);
        --i;
        continue;
      }
      h2_pal_net_resolve_close(net, resolver);
      if (rc != H2_PAL_OK)
        return rc;
    }
    return H2_PAL_OK;
  }
  case 5: {
    int socket = -1;
    rc = h2_pal_net_resolve_all(net, "::1", (h2_pal_net_family_t)99, &list);
    int opened =
        h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_ANY, NULL, &socket);
    h2_pal_net_close(net, socket);
    return rc == H2_PAL_ERR_INVALID_ARG && opened == H2_PAL_ERR_INVALID_ARG &&
                   socket < 0
               ? H2_PAL_OK
               : H2_PAL_ERR_FORMAT;
  }
  case 6: {
    int socket = -1;
    h2_pal_net_addr_t addr = {
        .family = H2_PAL_NET_FAMILY_IPV6, .port = 9u, .ip = {0xfe, 0x80}};
    rc = h2_pal_net_tcp_open_bound(net, addr.family, NULL, &socket);
    if (rc == H2_PAL_OK)
      rc = h2_pal_net_tcp_connect(net, socket, &addr, 0u);
    h2_pal_net_close(net, socket);
    return rc == H2_PAL_ERR_INVALID_ARG ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
  }
  case 7: {
    int socket = -1;
    h2_pal_net_addr_t bound, addr = {.family = H2_PAL_NET_FAMILY_IPV4,
                                     .port = 9u,
                                     .ip = {127, 0, 0, 1}};
    rc = h2_pal_net_udp_open_bound(net, H2_PAL_NET_FAMILY_IPV6, 0u, NULL,
                                   &socket, &bound);
    if (rc == H2_PAL_OK) {
      int sent =
          h2_pal_net_udp_sendto(net, socket, &addr, (const uint8_t *)"x", 1u);
      if (sent >= 0)
        rc = H2_PAL_ERR_FORMAT;
    }
    h2_pal_net_close(net, socket);
    return rc;
  }
  case 8: {
    h2_pal_net_addr_t addr;
    rc = h2_pal_net_get_host_addr_family(net, NULL, H2_PAL_NET_FAMILY_IPV6,
                                         &addr);
    if (rc == H2_PAL_OK && (addr.family != H2_PAL_NET_FAMILY_IPV6 ||
                            (addr.ip[0] == 0xfeu &&
                             (addr.ip[1] & 0xc0u) == 0x80u && !addr.scope_id)))
      rc = H2_PAL_ERR_FORMAT;
    return rc;
  }
  case 9:
    return http(config, config->http_url);
  case 10:
    return config->fallback_url && config->fallback_url[0]
               ? http(config, config->fallback_url)
               : http_local_fallback(config);
  case 11:
    return mqtt(config);
  case 12:
    return dtls(config, 0u);
  case 13:
    return dtls(config, 1u);
  case 14:
    return dtls(config, 2u);
  case 15:
    return webrtc(config);
  case 16:
    return bound_interface(config, 0);
  case 17:
    return bound_interface(config, 1);
  case 18: {
    if (!config->dns_server.port)
      return H2_PAL_ERR_UNAVAILABLE;
    char name[64];
    snprintf(name, sizeof(name), "%s.ipv6.test", config->transport.session);
    h2_dns_client_config_t client = {.net = net,
                                     .crypto = runtime->crypto,
                                     .time = runtime->time,
                                     .server = config->dns_server,
                                     .timeout_ms = 3000u,
                                     .retries = 0u};
    h2_dns_answer_t answer;
    size_t count = 0u;
    h2_dns_query_t query = {.name = name,
                            .type = H2_DNS_RECORD_AAAA,
                            .answers = &answer,
                            .max_answers = 1u,
                            .out_count = &count};
    rc = h2_dns_query(&client, &query);
    if (rc != H2_DNS_OK)
      return rc;
    return count == 1u && answer.type == H2_DNS_RECORD_AAAA &&
                   answer.addr.family == H2_PAL_NET_FAMILY_IPV6 &&
                   memcmp(answer.addr.ip, config->dns_answer.ip, 16u) == 0
               ? H2_PAL_OK
               : H2_PAL_ERR_FORMAT;
  }
  default:
    return H2_PAL_ERR_INVALID_ARG;
  }
}
int h2_pal_ipv6_e2e_run(const h2_pal_ipv6_config_t *config,
                        h2_pal_ipv6_result_t *out) {
  if (!config || !out || !config->transport.runtime)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  h2_net_tls_config_t transport = config->transport;
  transport.family = H2_PAL_NET_FAMILY_IPV6;
  transport.report = NULL;
  h2_net_tls_result_t raw;
  (void)h2_pal_net_tls_e2e_run(&transport, &raw);
  unsigned slot = 0u;
  for (unsigned i = 0u; i < H2_NET_TLS_CASE_COUNT; ++i)
    if (raw.cases[i].mandatory) {
      if (slot >= H2_PAL_IPV6_CASES - H2_PAL_IPV6_EXTRA_CASES)
        return H2_PAL_ERR_FORMAT;
      out->cases[slot++] = raw.cases[i];
    }
  if (slot != H2_PAL_IPV6_CASES - H2_PAL_IPV6_EXTRA_CASES)
    return H2_PAL_ERR_FORMAT;
  out->retained_sockets = raw.retained_sockets;
  out->retained_resolvers = raw.retained_resolvers;
  out->retained_allocations = raw.retained_allocations;
  for (unsigned i = 0u; i < H2_PAL_IPV6_EXTRA_CASES; ++i) {
    h2_net_tls_case_result_t *item = &out->cases[slot++];
    item->id = ids[i];
    item->mandatory = 1;
    item->detail = extra(config, i);
    item->passed = item->detail == H2_PAL_OK;
    item->blocked = item->detail == H2_PAL_ERR_UNAVAILABLE;
  }
  for (unsigned i = 0u; i < H2_PAL_IPV6_CASES; ++i) {
    h2_net_tls_case_result_t *item = &out->cases[i];
    if (item->passed)
      ++out->passed;
    else if (item->blocked)
      ++out->blocked;
    else
      ++out->failed;
    if (config->transport.report)
      config->transport.report(config->transport.report_user, item);
  }
  return !out->failed && !out->blocked && !out->retained_sockets &&
                 !out->retained_resolvers && !out->retained_allocations
             ? H2_PAL_OK
         : out->blocked ? H2_PAL_ERR_UNAVAILABLE
                        : H2_PAL_ERR_INVALID_STATE;
}
