#include "h2_pal_net_tls_e2e.h"
#include <string.h>

#define RESOLVER_LIMIT 32u
#define SOCKET_LIMIT 4u
#define PAYLOAD_BYTES 4097u
#define HEADER_BYTES 96u
#define IO_SLICE_MS 50u

static const struct {
  const char *id;
  int mandatory;
} registry[] = {
#define H2_NET_TLS_CASE(symbol, id, mandatory) {id, mandatory},
#include "h2_pal_net_tls_cases.inc"
#undef H2_NET_TLS_CASE
};

typedef struct state {
  const h2_net_tls_config_t *config;
  h2_net_tls_case_result_t *item;
  h2_pal_net_socket_t sockets[SOCKET_LIMIT];
  h2_pal_net_resolver_t *resolvers[RESOLVER_LIMIT];
  size_t sockets_owned, resolvers_owned;
  uint64_t started, deadline;
  uint8_t scratch[HEADER_BYTES + PAYLOAD_BYTES];
  uint8_t receive[257];
} state_t;

static uint64_t now(state_t *s) {
  uint64_t us = 0u;
  if (h2_pal_time_get_monotonic_us(s->config->runtime->time, &us) != H2_PAL_OK)
    return UINT64_MAX;
  return us / 1000u;
}
static uint32_t remaining(state_t *s) {
  uint64_t value = now(s);
  if (value >= s->deadline)
    return 0u;
  uint64_t left = s->deadline - value;
  return (uint32_t)(left > IO_SLICE_MS ? IO_SLICE_MS : left);
}
static int own_socket(state_t *s, int socket) {
  if (socket < 0 || s->sockets_owned == SOCKET_LIMIT)
    return H2_PAL_ERR_INVALID_STATE;
  for (size_t i = 0u; i < s->sockets_owned; ++i)
    if (s->sockets[i] == socket)
      return H2_PAL_OK;
  s->sockets[s->sockets_owned++] = socket;
  return H2_PAL_OK;
}
static void cleanup(state_t *s) {
  while (s->resolvers_owned)
    h2_pal_net_resolve_close(s->config->runtime->net,
                             s->resolvers[--s->resolvers_owned]);
  while (s->sockets_owned)
    h2_pal_net_close(s->config->runtime->net, s->sockets[--s->sockets_owned]);
}
static int address(state_t *s, uint16_t port, h2_pal_net_addr_t *out) {
  int rc =
      h2_pal_net_resolve_addr(s->config->runtime->net, s->config->host, out);
  if (rc == H2_PAL_OK)
    out->port = port;
  return rc;
}
static int start_peer(state_t *s, h2_net_tls_fixture_mode_t mode,
                      uint16_t callback_port, h2_pal_net_addr_t *out) {
  uint16_t port = 0u;
  int rc = s->config->prepare(s->config->fixture_user, s->item->id, mode,
                              callback_port, &port);
  if (rc != H2_PAL_OK)
    return rc;
  return address(s, port, out);
}
static int proof(state_t *s, h2_net_tls_fixture_proof_t kind) {
  return s->config->verify(s->config->fixture_user, s->item->id, kind);
}
static int connect_socket(state_t *s, const h2_pal_net_addr_t *addr, int bound,
                          int zero_start, int *out) {
  const h2_pal_net_api_t *net = s->config->runtime->net;
  h2_pal_net_bind_t bind = {.type = H2_PAL_NET_BIND_SOURCE_ADDR};
  int rc;
  if (bound) {
    rc = h2_pal_net_get_host_addr(net, NULL, &bind.source_addr);
    if (rc != H2_PAL_OK)
      return rc;
    bind.source_addr.port = 0u;
    /* An isolated loopback fixture must bind a local loopback source;
     * get_host_addr(NULL) can legitimately return the physical LAN IP. */
    if (addr->family == H2_PAL_NET_FAMILY_IPV4 && addr->ip[0] == 127u)
      memcpy(bind.source_addr.ip, addr->ip, sizeof(addr->ip));
    rc = h2_pal_net_tcp_open_bound(net, addr->family, &bind, out);
  } else
    rc = net->vtable->tcp_open(net->user, addr->family, out);
  if (rc != H2_PAL_OK)
    return rc;
  rc = own_socket(s, *out);
  if (rc != H2_PAL_OK)
    return rc;
  if (zero_start) {
    rc = h2_pal_net_tcp_connect(net, *out, addr, 0u);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_WOULD_BLOCK &&
        rc != H2_PAL_ERR_TIMEOUT)
      return rc;
    if (rc == H2_PAL_OK)
      return rc;
  }
  do {
    uint32_t slice = remaining(s);
    if (slice == 0u)
      return H2_PAL_ERR_TIMEOUT;
    rc = h2_pal_net_tcp_connect(net, *out, addr, slice);
  } while (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK);
  return rc;
}
static uint8_t payload(size_t index) {
  return (uint8_t)((index * 37u + 11u) % 251u);
}
static void packet(state_t *s) {
  memset(s->scratch, 0, HEADER_BYTES);
  memcpy(s->scratch, "H2NETTLS/1 ", 11u);
  memcpy(s->scratch + 11u, s->config->session, 32u);
  s->scratch[43] = ' ';
  size_t id_len = strlen(s->item->id);
  memcpy(s->scratch + 44u, s->item->id, id_len);
  for (size_t i = 0u; i < PAYLOAD_BYTES; ++i)
    s->scratch[HEADER_BYTES + i] = payload(i);
}
static int send_all(state_t *s, int socket, const uint8_t *bytes, size_t len,
                    int legacy) {
  size_t sent = 0u;
  while (sent < len) {
    uint32_t slice = remaining(s);
    if (!slice)
      return H2_PAL_ERR_TIMEOUT;
    size_t chunk = len - sent;
    if (chunk > 257u)
      chunk = 257u;
    int rc = legacy
                 ? h2_pal_net_tcp_send(s->config->runtime->net, socket,
                                       bytes + sent, chunk)
                 : h2_pal_net_tcp_send_timeout(s->config->runtime->net, socket,
                                               bytes + sent, chunk, slice);
    if (rc > 0 && (size_t)rc <= chunk) {
      sent += (size_t)rc;
      s->item->bytes_sent += (size_t)rc;
    } else if (rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_WOULD_BLOCK)
      return rc < 0 ? rc : H2_PAL_ERR_FORMAT;
  }
  return H2_PAL_OK;
}
static int receive_payload(state_t *s, int socket) {
  size_t received = 0u;
  while (received < PAYLOAD_BYTES) {
    uint32_t slice = remaining(s);
    if (!slice)
      return H2_PAL_ERR_TIMEOUT;
    size_t chunk = PAYLOAD_BYTES - received;
    if (chunk > sizeof(s->receive))
      chunk = sizeof(s->receive);
    int rc = h2_pal_net_tcp_recv(s->config->runtime->net, socket, s->receive,
                                 chunk, slice);
    if (rc > 0 && (size_t)rc <= chunk) {
      for (size_t i = 0u; i < (size_t)rc; ++i)
        if (s->receive[i] != (uint8_t)(payload(received + i) ^ 0xa5u))
          return H2_PAL_ERR_FORMAT;
      received += (size_t)rc;
      s->item->bytes_received += (size_t)rc;
    } else if (rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_WOULD_BLOCK)
      return rc < 0 ? rc : H2_PAL_ERR_FORMAT;
  }
  return H2_PAL_OK;
}
static int exchange(state_t *s, int socket, int legacy) {
  packet(s);
  int rc =
      send_all(s, socket, s->scratch, HEADER_BYTES + PAYLOAD_BYTES, legacy);
  return rc == H2_PAL_OK ? receive_payload(s, socket) : rc;
}
static int required_slots(const h2_pal_net_api_t *net) {
  if (!net || !net->vtable)
    return 0;
  const h2_pal_net_vtable_t *v = net->vtable;
  return v->resolve_addr && v->resolve_start && v->resolve_poll &&
         v->resolve_close && v->get_host_addr && v->udp_open &&
         v->udp_open_bound && v->udp_sendto && v->udp_recvfrom && v->tcp_open &&
         v->tcp_open_bound && v->tcp_connect && v->tcp_send &&
         v->tcp_send_timeout && v->tcp_recv && v->tcp_listen && v->tcp_accept &&
         v->tls_wrap && v->close;
}

#define CHECK(condition, error)                                                \
  do {                                                                         \
    if (!(condition)) {                                                        \
      s->item->detail = (error);                                               \
      s->item->line = __LINE__;                                                \
      goto done;                                                               \
    }                                                                          \
  } while (0)
#define OK(operation)                                                          \
  do {                                                                         \
    int check_rc = (operation);                                                \
    s->item->provider_result = check_rc;                                       \
    CHECK(check_rc == H2_PAL_OK, check_rc);                                    \
  } while (0)

static void run_case(state_t *s, h2_net_tls_case_t kind) {
  const h2_pal_net_api_t *net = s->config->runtime->net;
  h2_pal_net_addr_t addr = {0}, bound_addr = {0}, peer = {0};
  int socket = -1, rc = H2_PAL_OK;
  int tls_socket = -1;
  h2_pal_net_tls_config_t tls = {.server_name = s->config->server_name,
                                 .root_ca_pem = s->config->root_ca,
                                 .root_ca_pem_len = s->config->root_ca_len,
                                 .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED};
  switch (kind) {
  case H2_NET_TLS_WRAPPER_ARGUMENTS: {
    CHECK(h2_pal_net_resolve_addr(NULL, "x", &addr) == H2_PAL_ERR_INVALID_ARG,
          H2_PAL_ERR_FORMAT);
    CHECK(h2_pal_net_tcp_open_bound(NULL, H2_PAL_NET_FAMILY_IPV4, NULL,
                                    &socket) == H2_PAL_ERR_INVALID_ARG,
          H2_PAL_ERR_FORMAT);
    CHECK(h2_pal_net_tls_wrap(NULL, -1, &tls, 1u, &socket) ==
              H2_PAL_ERR_INVALID_ARG,
          H2_PAL_ERR_FORMAT);
    CHECK(h2_pal_net_resolve_start(net, NULL, NULL) == H2_PAL_ERR_INVALID_ARG,
          H2_PAL_ERR_FORMAT);
    break;
  }
  case H2_NET_TLS_DNS_SYNC:
    OK(address(s, 0u, &addr));
    CHECK(addr.family == H2_PAL_NET_FAMILY_IPV4, H2_PAL_ERR_FORMAT);
    break;
  case H2_NET_TLS_DNS_ASYNC_COPY: {
    char host[64];
    CHECK(strlen(s->config->host) < sizeof(host), H2_PAL_ERR_INVALID_ARG);
    strcpy(host, s->config->host);
    h2_pal_net_resolver_t *resolver = NULL;
    OK(h2_pal_net_resolve_start(net, host, &resolver));
    CHECK(resolver != NULL, H2_PAL_ERR_INVALID_STATE);
    s->resolvers[s->resolvers_owned++] = resolver;
    memset(host, 'x', strlen(host));
    do {
      uint32_t slice = remaining(s);
      CHECK(slice, H2_PAL_ERR_TIMEOUT);
      rc = h2_pal_net_resolve_poll(net, resolver, &addr, slice);
    } while (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK);
    CHECK(rc == H2_PAL_OK, rc);
    OK(address(s, 0u, &peer));
    CHECK(addr.family == peer.family && memcmp(addr.ip, peer.ip, 16u) == 0,
          H2_PAL_ERR_FORMAT);
    break;
  }
  case H2_NET_TLS_DNS_CANCEL:
    for (unsigned i = 0u; i < 16u; ++i) {
      h2_pal_net_resolver_t *resolver = NULL;
      rc = h2_pal_net_resolve_start(net, s->config->host, &resolver);
      if (rc == H2_PAL_ERR_NO_SPACE) {
        CHECK(remaining(s), H2_PAL_ERR_TIMEOUT);
        --i;
        (void)h2_pal_time_sleep_ms(s->config->runtime->time, 5u);
        continue;
      }
      CHECK(rc == H2_PAL_OK && resolver != NULL, rc);
      h2_pal_net_resolve_close(net, resolver);
    }
    break;
  case H2_NET_TLS_DNS_CAPACITY:
    while (s->resolvers_owned < RESOLVER_LIMIT) {
      h2_pal_net_resolver_t *resolver = NULL;
      rc = h2_pal_net_resolve_start(net, s->config->host, &resolver);
      if (rc == H2_PAL_ERR_NO_SPACE && s->resolvers_owned == 0u) {
        /* Canceled lookups can become backend-owned until workers finish.
         * Wait for that *previous* lifetime to release capacity, then test
         * this case's own bounded capacity. */
        CHECK(remaining(s), H2_PAL_ERR_TIMEOUT);
        OK(h2_pal_time_sleep_ms(s->config->runtime->time, 20u));
        continue;
      }
      if (rc == H2_PAL_ERR_NO_SPACE) {
        CHECK(resolver == NULL, H2_PAL_ERR_FORMAT);
        break;
      }
      CHECK(rc == H2_PAL_OK && resolver != NULL, rc);
      s->resolvers[s->resolvers_owned++] = resolver;
    }
    CHECK(s->resolvers_owned > 0u && rc == H2_PAL_ERR_NO_SPACE,
          H2_PAL_ERR_NO_SPACE);
    break;
  case H2_NET_TLS_HOST_ADDRESS:
    OK(h2_pal_net_get_host_addr(net, NULL, &addr));
    CHECK(addr.family == H2_PAL_NET_FAMILY_IPV4, H2_PAL_ERR_FORMAT);
    rc =
        h2_pal_net_get_host_addr(net, "h2-definitely-missing-interface", &peer);
    CHECK(rc == H2_PAL_ERR_NOT_FOUND || rc == H2_PAL_ERR_UNSUPPORTED, rc);
    break;
  case H2_NET_TLS_UDP_ECHO:
  case H2_NET_TLS_UDP_SOURCE_BIND:
  case H2_NET_TLS_UDP_TIMEOUT:
  case H2_NET_TLS_UDP_NONBLOCKING:
  case H2_NET_TLS_UDP_TRUNCATION:
  case H2_NET_TLS_MULTICAST: {
    if (kind == H2_NET_TLS_MULTICAST && !s->config->multicast_supported) {
      if (net->vtable->udp_join_multicast)
        s->item->not_assessed = 1;
      else
        s->item->unsupported = 1;
      s->item->detail = H2_PAL_ERR_UNSUPPORTED;
      goto done;
    }
    h2_pal_net_bind_t bind = {.type = H2_PAL_NET_BIND_SOURCE_ADDR};
    if (kind == H2_NET_TLS_UDP_SOURCE_BIND) {
      OK(h2_pal_net_get_host_addr(net, NULL, &bind.source_addr));
      bind.source_addr.port = 0u;
      OK(address(s, 0u, &addr));
      if (addr.family == H2_PAL_NET_FAMILY_IPV4 && addr.ip[0] == 127u)
        memcpy(bind.source_addr.ip, addr.ip, sizeof(addr.ip));
      OK(h2_pal_net_udp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, 0u, &bind,
                                   &socket, &bound_addr));
    } else
      OK(net->vtable->udp_open(net->user, H2_PAL_NET_FAMILY_IPV4, 0u, &socket,
                               &bound_addr));
    OK(own_socket(s, socket));
    CHECK(bound_addr.port != 0u, H2_PAL_ERR_FORMAT);
    if (kind == H2_NET_TLS_UDP_SOURCE_BIND) {
      CHECK(bound_addr.family == bind.source_addr.family &&
                memcmp(bound_addr.ip, bind.source_addr.ip,
                       sizeof(bound_addr.ip)) == 0,
            H2_PAL_ERR_FORMAT);
      bind.type = (h2_pal_net_bind_type_t)99;
      int invalid_socket = -1;
      rc = h2_pal_net_udp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, 0u, &bind,
                                     &invalid_socket, &peer);
      if (invalid_socket >= 0)
        OK(own_socket(s, invalid_socket));
      CHECK(rc == H2_PAL_ERR_UNSUPPORTED || rc == H2_PAL_ERR_INVALID_ARG,
            H2_PAL_ERR_FORMAT);
    }
    if (kind == H2_NET_TLS_MULTICAST) {
      addr.family = H2_PAL_NET_FAMILY_IPV4;
      addr.ip[0] = 239u;
      addr.ip[3] = 77u;
      OK(h2_pal_net_udp_join_multicast(net, socket, &addr));
      break;
    }
    if (kind == H2_NET_TLS_UDP_TIMEOUT || kind == H2_NET_TLS_UDP_NONBLOCKING) {
      uint32_t wait = kind == H2_NET_TLS_UDP_NONBLOCKING ? 0u : 40u;
      rc = h2_pal_net_udp_recvfrom(net, socket, &peer, s->receive,
                                   sizeof(s->receive), wait);
      CHECK(wait ? rc == H2_PAL_ERR_TIMEOUT
                 : (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT),
            rc);
      CHECK(now(s) - s->started <= wait + 250u, H2_PAL_ERR_TIMEOUT);
      break;
    }
    OK(start_peer(s, H2_NET_TLS_FIXTURE_UDP, 0u, &addr));
    packet(s);
    size_t len = HEADER_BYTES + 513u;
    rc = h2_pal_net_udp_sendto(net, socket, &addr, s->scratch, len);
    CHECK(rc == (int)len, rc);
    s->item->bytes_sent += len;
    size_t receive_len = kind == H2_NET_TLS_UDP_TRUNCATION ? 17u : 513u;
    do {
      uint32_t slice = remaining(s);
      CHECK(slice, H2_PAL_ERR_TIMEOUT);
      rc = h2_pal_net_udp_recvfrom(net, socket, &peer, s->scratch, receive_len,
                                   slice);
    } while (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK);
    CHECK(rc == (int)receive_len && peer.port == addr.port &&
              memcmp(peer.ip, addr.ip, 16u) == 0,
          rc);
    for (size_t i = 0u; i < receive_len; ++i)
      CHECK(s->scratch[i] == (uint8_t)(payload(i) ^ 0xa5u), H2_PAL_ERR_FORMAT);
    s->item->bytes_received += receive_len;
    OK(proof(s, H2_NET_TLS_PROOF_PAYLOAD));
    break;
  }
  case H2_NET_TLS_TCP_LISTEN_ACCEPT:
  case H2_NET_TLS_TCP_ACCEPT_TIMEOUT: {
    OK(h2_pal_net_tcp_listen(net, H2_PAL_NET_FAMILY_IPV4, 0u, NULL, &socket,
                             &bound_addr));
    OK(own_socket(s, socket));
    CHECK(bound_addr.port != 0u, H2_PAL_ERR_FORMAT);
    if (kind == H2_NET_TLS_TCP_ACCEPT_TIMEOUT) {
      rc = h2_pal_net_tcp_accept(net, socket, &tls_socket, &peer, 40u);
      CHECK(rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK, rc);
      CHECK(now(s) - s->started <= 300u, H2_PAL_ERR_TIMEOUT);
    }
    OK(start_peer(s, H2_NET_TLS_FIXTURE_CALLBACK, bound_addr.port, &addr));
    do {
      uint32_t slice = remaining(s);
      CHECK(slice, H2_PAL_ERR_TIMEOUT);
      rc = h2_pal_net_tcp_accept(net, socket, &tls_socket, &peer, slice);
    } while (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK);
    CHECK(rc == H2_PAL_OK, rc);
    OK(own_socket(s, tls_socket));
    CHECK(peer.family == H2_PAL_NET_FAMILY_IPV4, H2_PAL_ERR_FORMAT);
    OK(exchange(s, tls_socket, 0));
    OK(proof(s, H2_NET_TLS_PROOF_PAYLOAD));
    break;
  }
  case H2_NET_TLS_TCP_ECHO:
  case H2_NET_TLS_TCP_SOURCE_BIND:
  case H2_NET_TLS_TCP_CONNECT_RETRY:
  case H2_NET_TLS_TCP_TIMEOUT:
  case H2_NET_TLS_TCP_NONBLOCKING:
  case H2_NET_TLS_TCP_PEER_CLOSE:
  case H2_NET_TLS_TCP_PARTIAL_IO: {
    h2_net_tls_fixture_mode_t mode = H2_NET_TLS_FIXTURE_TCP;
    if (kind == H2_NET_TLS_TCP_TIMEOUT || kind == H2_NET_TLS_TCP_NONBLOCKING)
      mode = H2_NET_TLS_FIXTURE_SILENT;
    if (kind == H2_NET_TLS_TCP_PEER_CLOSE)
      mode = H2_NET_TLS_FIXTURE_CLOSE;
    OK(start_peer(s, mode, 0u, &addr));
    OK(connect_socket(s, &addr, kind == H2_NET_TLS_TCP_SOURCE_BIND,
                      kind == H2_NET_TLS_TCP_CONNECT_RETRY, &socket));
    if (kind == H2_NET_TLS_TCP_SOURCE_BIND) {
      h2_pal_net_bind_t invalid = {
          .type = (h2_pal_net_bind_type_t)99,
          .source_addr = {.family = H2_PAL_NET_FAMILY_IPV4}};
      int invalid_socket = -1;
      rc = h2_pal_net_tcp_open_bound(net, H2_PAL_NET_FAMILY_IPV4, &invalid,
                                     &invalid_socket);
      if (invalid_socket >= 0)
        OK(own_socket(s, invalid_socket));
      CHECK(rc == H2_PAL_ERR_UNSUPPORTED || rc == H2_PAL_ERR_INVALID_ARG,
            H2_PAL_ERR_FORMAT);
    }
    if (mode == H2_NET_TLS_FIXTURE_SILENT) {
      uint64_t before = now(s);
      uint32_t wait = kind == H2_NET_TLS_TCP_NONBLOCKING ? 0u : 40u;
      rc = h2_pal_net_tcp_recv(net, socket, s->receive, sizeof(s->receive),
                               wait);
      CHECK(wait ? rc == H2_PAL_ERR_TIMEOUT
                 : (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT),
            rc);
      CHECK(now(s) - before <= wait + 250u, H2_PAL_ERR_TIMEOUT);
      OK(proof(s, H2_NET_TLS_PROOF_CONNECTED));
    } else if (mode == H2_NET_TLS_FIXTURE_CLOSE) {
      do {
        uint32_t slice = remaining(s);
        CHECK(slice, H2_PAL_ERR_TIMEOUT);
        rc = h2_pal_net_tcp_recv(net, socket, s->receive, sizeof(s->receive),
                                 slice);
      } while (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK);
      CHECK(rc == H2_PAL_ERR_CLOSED, rc);
      OK(proof(s, H2_NET_TLS_PROOF_CLOSED));
    } else {
      OK(exchange(s, socket, kind == H2_NET_TLS_TCP_ECHO));
      OK(proof(s, H2_NET_TLS_PROOF_PAYLOAD));
    }
    break;
  }
  case H2_NET_TLS_DNS_HOSTNAME: {
    OK(h2_pal_net_resolve_addr(net, s->config->dns_host, &addr));
    CHECK(addr.family == s->config->dns_expected.family &&
              memcmp(addr.ip, s->config->dns_expected.ip, sizeof(addr.ip)) == 0,
          H2_PAL_ERR_FORMAT);
    memcpy(s->item->observed_ipv4, addr.ip, sizeof(s->item->observed_ipv4));
    char host[64];
    CHECK(strlen(s->config->dns_host) < sizeof(host), H2_PAL_ERR_INVALID_ARG);
    strcpy(host, s->config->dns_host);
    h2_pal_net_resolver_t *resolver = NULL;
    OK(h2_pal_net_resolve_start(net, host, &resolver));
    CHECK(resolver != NULL, H2_PAL_ERR_INVALID_STATE);
    s->resolvers[s->resolvers_owned++] = resolver;
    memset(host, 'x', strlen(host));
    do {
      uint32_t slice = remaining(s);
      CHECK(slice, H2_PAL_ERR_TIMEOUT);
      rc = h2_pal_net_resolve_poll(net, resolver, &addr, slice);
    } while (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK);
    CHECK(rc == H2_PAL_OK && addr.family == s->config->dns_expected.family &&
              memcmp(addr.ip, s->config->dns_expected.ip, sizeof(addr.ip)) == 0,
          rc == H2_PAL_OK ? H2_PAL_ERR_FORMAT : rc);
    h2_pal_net_resolve_close(net, s->resolvers[--s->resolvers_owned]);
    memcpy(host, s->config->session, 32u);
    strcpy(host + 32u, ".invalid");
    OK(h2_pal_net_resolve_start(net, host, &resolver));
    CHECK(resolver != NULL, H2_PAL_ERR_INVALID_STATE);
    s->resolvers[s->resolvers_owned++] = resolver;
    do {
      uint32_t slice = remaining(s);
      CHECK(slice, H2_PAL_ERR_TIMEOUT);
      rc = h2_pal_net_resolve_poll(net, resolver, &addr, slice);
    } while (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK);
    CHECK(rc == H2_PAL_ERR_NOT_FOUND, rc);
    s->item->provider_result = rc;
    break;
  }
  case H2_NET_TLS_ICMP: {
    if (!s->config->icmp_supported) {
      if (net->vtable->icmp_echo)
        s->item->not_assessed = 1;
      else
        s->item->unsupported = 1;
      s->item->detail = H2_PAL_ERR_UNSUPPORTED;
      goto done;
    }
    OK(address(s, 0u, &addr));
    h2_pal_net_icmp_echo_result_t ping = {0};
    OK(h2_pal_net_icmp_echo(net, &addr, NULL, 1000u, &ping));
    CHECK(ping.transmitted && ping.received &&
              ping.received <= ping.transmitted,
          H2_PAL_ERR_FORMAT);
    break;
  }
  case H2_NET_TLS_CLEANUP: {
    CHECK(s->sockets_owned == 0u && s->resolvers_owned == 0u,
          H2_PAL_ERR_INVALID_STATE);
    /* Capacity qualification held every resolver earlier. A new terminal
     * lookup here must succeed after their ownership was released. */
    h2_pal_net_resolver_t *resolver = NULL;
    OK(h2_pal_net_resolve_start(net, s->config->host, &resolver));
    CHECK(resolver != NULL, H2_PAL_ERR_INVALID_STATE);
    s->resolvers[s->resolvers_owned++] = resolver;
    do {
      uint32_t slice = remaining(s);
      CHECK(slice, H2_PAL_ERR_TIMEOUT);
      rc = h2_pal_net_resolve_poll(net, resolver, &addr, slice);
    } while (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK);
    CHECK(rc == H2_PAL_OK, rc);
    break;
  }
  default: {
    unsigned repeats = kind == H2_NET_TLS_TLS_SESSION_CHURN      ? 12u
                       : kind == H2_NET_TLS_TLS_FAILURE_RECOVERY ? 2u
                                                                 : 1u;
    for (unsigned iteration = 0u; iteration < repeats; ++iteration) {
      int rejected =
          kind == H2_NET_TLS_TLS_DEFAULT_UNTRUSTED ||
          kind == H2_NET_TLS_TLS_WRONG_CA ||
          kind == H2_NET_TLS_TLS_WRONG_NAME || kind == H2_NET_TLS_TLS_EXPIRED ||
          (kind == H2_NET_TLS_TLS_FAILURE_RECOVERY && iteration == 0u);
      h2_net_tls_fixture_mode_t mode =
          kind == H2_NET_TLS_TLS_EXPIRED || kind == H2_NET_TLS_TLS_INSECURE
              ? H2_NET_TLS_FIXTURE_EXPIRED
          : kind == H2_NET_TLS_TLS_HANDSHAKE_DEADLINE
              ? H2_NET_TLS_FIXTURE_SILENT
              : H2_NET_TLS_FIXTURE_TLS;
      tls.verify = kind == H2_NET_TLS_TLS_DEFAULT ||
                           kind == H2_NET_TLS_TLS_DEFAULT_UNTRUSTED
                       ? H2_PAL_NET_TLS_VERIFY_DEFAULT
                   : kind == H2_NET_TLS_TLS_INSECURE
                       ? H2_PAL_NET_TLS_VERIFY_INSECURE_TEST_ONLY
                       : H2_PAL_NET_TLS_VERIFY_REQUIRED;
      tls.server_name =
          kind == H2_NET_TLS_TLS_WRONG_NAME || kind == H2_NET_TLS_TLS_INSECURE
              ? "wrong-pal-net-tls.test"
              : s->config->server_name;
      tls.root_ca_pem =
          kind == H2_NET_TLS_TLS_WRONG_CA ||
                  kind == H2_NET_TLS_TLS_DEFAULT_UNTRUSTED ||
                  kind == H2_NET_TLS_TLS_INSECURE ||
                  (kind == H2_NET_TLS_TLS_FAILURE_RECOVERY && iteration == 0u)
              ? s->config->wrong_ca
              : s->config->root_ca;
      tls.root_ca_pem_len = tls.root_ca_pem == s->config->wrong_ca
                                ? s->config->wrong_ca_len
                                : s->config->root_ca_len;
      tls.alpn = kind == H2_NET_TLS_TLS_SNI_ALPN ? "h2-pal-e2e" : NULL;
      OK(start_peer(s, mode, 0u, &addr));
      OK(connect_socket(s, &addr, 0, 0, &socket));
      if (kind == H2_NET_TLS_TLS_CONFIG_ARGUMENTS) {
        tls.verify = (h2_pal_net_tls_verify_t)99;
        rc = h2_pal_net_tls_wrap(net, socket, &tls, 500u, &tls_socket);
        s->item->provider_result = rc;
        CHECK(rc == H2_PAL_ERR_INVALID_ARG && tls_socket < 0, rc);
        break;
      }
      if (kind == H2_NET_TLS_TLS_INVALID_CA) {
        tls.root_ca_pem = (const uint8_t *)"malformed test certificate";
        tls.root_ca_pem_len = 26u;
        rc = h2_pal_net_tls_wrap(net, socket, &tls, 500u, &tls_socket);
        s->item->provider_result = rc;
        CHECK(rc == H2_PAL_ERR_FORMAT && tls_socket < 0, rc);
        break;
      }
      if (kind == H2_NET_TLS_TLS_BORROWED_CONFIG) {
        CHECK(tls.root_ca_pem_len + strlen(tls.server_name) + 2u <
                  sizeof(s->scratch),
              H2_PAL_ERR_NO_SPACE);
        memcpy(s->scratch, tls.root_ca_pem, tls.root_ca_pem_len);
        char *name = (char *)s->scratch + tls.root_ca_pem_len + 1u;
        strcpy(name, tls.server_name);
        tls.root_ca_pem = s->scratch;
        tls.server_name = name;
      }
      uint64_t before = now(s);
      uint32_t wait = kind == H2_NET_TLS_TLS_HANDSHAKE_DEADLINE ? 100u : 3000u;
      rc = h2_pal_net_tls_wrap(net, socket, &tls, wait, &tls_socket);
      s->item->provider_result = rc;
      CHECK(now(s) - before <= wait + 500u, H2_PAL_ERR_TIMEOUT);
      if (mode == H2_NET_TLS_FIXTURE_SILENT) {
        CHECK(rc == H2_PAL_ERR_TIMEOUT && tls_socket < 0, rc);
        OK(proof(s, H2_NET_TLS_PROOF_SILENT));
        s->item->provider_result = rc;
      } else if (rejected) {
        CHECK(rc == H2_PAL_ERR_TLS_VERIFY && tls_socket < 0, rc);
        /* Proof requires the exact armed peer's ClientHello, server
         * Certificate and completed failed handshake, no app payload. */
        cleanup(s);
        OK(proof(s, H2_NET_TLS_PROOF_CERT_REJECTION));
        s->item->provider_result = rc;
      } else {
        CHECK(rc == H2_PAL_OK && tls_socket >= 0, rc);
        OK(own_socket(s, tls_socket));
        if (kind == H2_NET_TLS_TLS_REPEAT_WRAP) {
          int duplicate = -1;
          rc = h2_pal_net_tls_wrap(net, socket, &tls, 500u, &duplicate);
          CHECK(rc == H2_PAL_ERR_INVALID_STATE && duplicate < 0, rc);
        }
        if (kind == H2_NET_TLS_TLS_BORROWED_CONFIG) {
          /* Configuration remains valid throughout synchronous wrap,
           * then the caller overwrites its private copies below. */
          memset(s->scratch, 0xa5, sizeof(s->scratch));
          tls.server_name = "retired";
          tls.alpn = "retired";
          tls.root_ca_pem = NULL;
          tls.root_ca_pem_len = 0u;
        }
        OK(exchange(s, tls_socket, 0));
        OK(proof(s, kind == H2_NET_TLS_TLS_SNI_ALPN
                        ? H2_NET_TLS_PROOF_SNI_ALPN
                        : H2_NET_TLS_PROOF_PAYLOAD));
      }
      cleanup(s);
    }
    break;
  }
  }
  s->item->passed = 1;
done:
  cleanup(s);
  s->item->elapsed_ms = now(s) - s->started;
  if (s->item->elapsed_ms > (uint64_t)(s->config->case_timeout_ms
                                           ? s->config->case_timeout_ms
                                           : 10000u)) {
    s->item->passed = 0;
    s->item->detail = H2_PAL_ERR_TIMEOUT;
  }
}

int h2_pal_net_tls_e2e_run(const h2_net_tls_config_t *config,
                           h2_net_tls_result_t *out) {
  if (!out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  int ready = config && config->runtime && config->runtime->mem &&
              config->runtime->mem->vtable &&
              config->runtime->mem->vtable->alloc &&
              config->runtime->mem->vtable->free && config->runtime->time &&
              config->runtime->time->vtable &&
              config->runtime->time->vtable->get_monotonic_us &&
              config->runtime->time->vtable->sleep_ms &&
              required_slots(config->runtime->net) && config->host &&
              config->dns_host && config->dns_host[0] &&
              config->dns_expected.family == H2_PAL_NET_FAMILY_IPV4 &&
              config->session && strlen(config->session) == 32u &&
              config->server_name && config->root_ca && config->root_ca_len &&
              config->wrong_ca && config->wrong_ca_len && config->prepare &&
              config->verify;
  state_t *s =
      ready ? h2_pal_mem_alloc(config->runtime->mem, sizeof(*s)) : NULL;
  if (s) {
    memset(s, 0, sizeof(*s));
    s->config = config;
  }
  for (unsigned i = 0u; i < H2_NET_TLS_CASE_COUNT; ++i) {
    h2_net_tls_case_result_t *item = &out->cases[i];
    item->id = registry[i].id;
    item->mandatory = registry[i].mandatory;
    if (!ready || !s) {
      item->blocked = 1;
      item->detail = ready ? H2_PAL_ERR_NO_MEMORY : H2_PAL_ERR_UNSUPPORTED;
    } else {
      s->item = item;
      s->started = now(s);
      s->deadline =
          s->started +
          (config->case_timeout_ms ? config->case_timeout_ms : 10000u);
      run_case(s, (h2_net_tls_case_t)i);
    }
    if (item->passed) {
      ++out->passed;
      if (item->mandatory)
        ++out->mandatory_passed;
    } else if (item->blocked)
      ++out->blocked;
    else if (item->unsupported)
      ++out->unsupported;
    else if (item->not_assessed)
      ++out->not_assessed;
    else
      ++out->failed;
    if (config && config->report)
      config->report(config->report_user, item);
  }
  if (s) {
    out->retained_sockets = s->sockets_owned;
    out->retained_resolvers = s->resolvers_owned;
    h2_pal_mem_free(config->runtime->mem, s);
  }
  return out->failed || out->blocked || out->retained_sockets ||
                 out->retained_resolvers
             ? H2_PAL_ERR_IO
             : H2_PAL_OK;
}
