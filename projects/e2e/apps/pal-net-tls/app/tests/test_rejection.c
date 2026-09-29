#include "h2_pal_net_tls_e2e.h"
#include <stdlib.h>
#include <string.h>

struct h2_pal_net_resolver {
  char host[64];
};
typedef struct fake {
  h2_net_tls_case_t current;
  unsigned prepare_count, active_sockets, active_resolvers, allocations;
  size_t written, received;
  uint64_t clock_us;
  int generic_tls_error, bad_proof, corrupt, fail_allocate, fail_prepare;
  int sockets[128], next_socket, wrapped;
  h2_net_tls_fixture_mode_t mode;
} fake_t;
static uint8_t pattern(size_t index) {
  return (uint8_t)((index * 37u + 11u) % 251u);
}
static void *allocate(void *user, size_t length) {
  fake_t *f = user;
  if (f->fail_allocate)
    return NULL;
  void *out = malloc(length);
  if (out)
    ++f->allocations;
  return out;
}
static void release(void *user, void *ptr) {
  fake_t *f = user;
  if (ptr) {
    --f->allocations;
    free(ptr);
  }
}
static h2_pal_result_t monotonic(void *user, uint64_t *out) {
  fake_t *f = user;
  *out = f->clock_us;
  return H2_PAL_OK;
}
static h2_pal_result_t sleep_ms(void *user, uint32_t ms) {
  fake_t *f = user;
  f->clock_us += (uint64_t)ms * 1000u;
  return H2_PAL_OK;
}
static int resolve(void *user, const char *host, h2_pal_net_addr_t *out) {
  (void)user;
  if (strcmp(host, "127.0.0.1") && strcmp(host, "ap.e2e.gizclaw.com"))
    return H2_PAL_ERR_NOT_FOUND;
  memset(out, 0, sizeof(*out));
  out->family = H2_PAL_NET_FAMILY_IPV4;
  out->ip[0] = 127u;
  out->ip[3] = 1u;
  if (strcmp(host, "ap.e2e.gizclaw.com") == 0) {
    out->ip[0] = 150u;
    out->ip[1] = 5u;
    out->ip[2] = 151u;
    out->ip[3] = 236u;
  }
  return H2_PAL_OK;
}
static h2_pal_result_t resolve_start(void *user, const char *host,
                                     h2_pal_net_resolver_t **out) {
  fake_t *f = user;
  *out = NULL;
  if (f->active_resolvers == 8u)
    return H2_PAL_ERR_NO_SPACE;
  *out = malloc(sizeof(**out));
  if (!*out)
    return H2_PAL_ERR_NO_MEMORY;
  strcpy((*out)->host, host);
  ++f->active_resolvers;
  return H2_PAL_OK;
}
static h2_pal_result_t resolve_poll(void *user, h2_pal_net_resolver_t *resolver,
                                    h2_pal_net_addr_t *out, uint32_t ms) {
  (void)ms;
  return (h2_pal_result_t)resolve(user, resolver->host, out);
}
static void resolve_close(void *user, h2_pal_net_resolver_t *resolver) {
  fake_t *f = user;
  --f->active_resolvers;
  free(resolver);
}
static int host_addr(void *user, const char *prefix, h2_pal_net_addr_t *out) {
  return prefix && *prefix ? H2_PAL_ERR_NOT_FOUND
                           : resolve(user, "127.0.0.1", out);
}
static int open_socket(void *user, h2_pal_net_family_t family, int *out) {
  fake_t *f = user;
  (void)family;
  *out = ++f->next_socket;
  if (*out >= 128)
    return H2_PAL_ERR_NO_SPACE;
  f->sockets[*out] = 1;
  ++f->active_sockets;
  return H2_PAL_OK;
}
static int open_bound(void *user, h2_pal_net_family_t family,
                      const h2_pal_net_bind_t *bind, int *out) {
  if (bind && (int)bind->type == 99) {
    *out = -1;
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return open_socket(user, family, out);
}
static int udp_open(void *user, h2_pal_net_family_t family, uint16_t port,
                    int *out, h2_pal_net_addr_t *addr) {
  (void)port;
  resolve(user, "127.0.0.1", addr);
  addr->port = 123u;
  return open_socket(user, family, out);
}
static int udp_bound(void *user, h2_pal_net_family_t family, uint16_t port,
                     const h2_pal_net_bind_t *bind, int *out,
                     h2_pal_net_addr_t *addr) {
  if (bind && (int)bind->type == 99) {
    *out = -1;
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return udp_open(user, family, port, out, addr);
}
static int udp_send(void *user, int socket, const h2_pal_net_addr_t *addr,
                    const uint8_t *bytes, size_t len) {
  (void)user;
  (void)socket;
  (void)addr;
  (void)bytes;
  return (int)len;
}
static int udp_recv(void *user, int socket, h2_pal_net_addr_t *addr,
                    uint8_t *bytes, size_t len, uint32_t ms) {
  fake_t *f = user;
  (void)socket;
  if (f->current == H2_NET_TLS_UDP_TIMEOUT ||
      f->current == H2_NET_TLS_UDP_NONBLOCKING) {
    f->clock_us += (uint64_t)ms * 1000u;
    return ms ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_WOULD_BLOCK;
  }
  resolve(user, "127.0.0.1", addr);
  addr->port = 123u;
  for (size_t i = 0; i < len; ++i)
    bytes[i] = (uint8_t)(pattern(i) ^ 0xa5u);
  return (int)len;
}
static int multicast(void *user, int socket, const h2_pal_net_addr_t *addr) {
  (void)user;
  (void)socket;
  (void)addr;
  return H2_PAL_OK;
}
static h2_pal_result_t connect_socket(void *user, int socket,
                                      const h2_pal_net_addr_t *addr,
                                      uint32_t ms) {
  (void)user;
  (void)socket;
  (void)addr;
  return ms ? H2_PAL_OK : H2_PAL_ERR_WOULD_BLOCK;
}
static int send_bytes(void *user, int socket, const uint8_t *bytes,
                      size_t len) {
  fake_t *f = user;
  (void)socket;
  size_t short_write = len > 17u ? 17u : len;
  for (size_t i = 0; i < short_write; ++i) {
    if (f->written + i >= 96u && bytes[i] != pattern(f->written + i - 96u))
      return H2_PAL_ERR_FORMAT;
  }
  f->written += short_write;
  return (int)short_write;
}
static int send_timeout(void *user, int socket, const uint8_t *bytes,
                        size_t len, uint32_t ms) {
  (void)ms;
  return send_bytes(user, socket, bytes, len);
}
static int receive_bytes(void *user, int socket, uint8_t *bytes, size_t len,
                         uint32_t ms) {
  fake_t *f = user;
  (void)socket;
  if (f->current == H2_NET_TLS_TCP_TIMEOUT ||
      f->current == H2_NET_TLS_TCP_NONBLOCKING) {
    f->clock_us += (uint64_t)ms * 1000u;
    return ms ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_WOULD_BLOCK;
  }
  if (f->current == H2_NET_TLS_TCP_PEER_CLOSE)
    return H2_PAL_ERR_CLOSED;
  size_t short_read = len > 13u ? 13u : len;
  for (size_t i = 0; i < short_read; ++i)
    bytes[i] = (uint8_t)(pattern(f->received + i) ^ 0xa5u);
  if (f->corrupt && f->current == H2_NET_TLS_TLS_REQUIRED)
    bytes[0] ^= 1u;
  f->received += short_read;
  return (int)short_read;
}
static h2_pal_result_t wrap(void *user, int socket,
                            const h2_pal_net_tls_config_t *config, uint32_t ms,
                            int *out) {
  fake_t *f = user;
  *out = -1;
  if (f->wrapped)
    return H2_PAL_ERR_INVALID_STATE;
  if ((int)config->verify == 99)
    return H2_PAL_ERR_INVALID_ARG;
  if (config->root_ca_pem_len == 26u)
    return H2_PAL_ERR_FORMAT;
  if (f->current == H2_NET_TLS_TLS_HANDSHAKE_DEADLINE) {
    f->clock_us += (uint64_t)ms * 1000u;
    return H2_PAL_ERR_TIMEOUT;
  }
  int rejected =
      f->current == H2_NET_TLS_TLS_DEFAULT_UNTRUSTED ||
      f->current == H2_NET_TLS_TLS_WRONG_CA ||
      f->current == H2_NET_TLS_TLS_WRONG_NAME ||
      f->current == H2_NET_TLS_TLS_EXPIRED ||
      (f->current == H2_NET_TLS_TLS_FAILURE_RECOVERY && f->prepare_count == 1u);
  if (rejected)
    return f->generic_tls_error ? H2_PAL_ERR_IO : H2_PAL_ERR_TLS_VERIFY;
  f->wrapped = 1;
  *out = socket;
  return H2_PAL_OK;
}
static void close_socket(void *user, int socket) {
  fake_t *f = user;
  if (socket < 0 || socket >= 128 || !f->sockets[socket])
    abort();
  f->sockets[socket] = 0;
  --f->active_sockets;
  f->wrapped = 0;
}
static int listen_socket(void *user, h2_pal_net_family_t family, uint16_t port,
                         const h2_pal_net_bind_t *bind, int *out,
                         h2_pal_net_addr_t *addr) {
  if (bind && (int)bind->type == 99) {
    *out = -1;
    return H2_PAL_ERR_UNSUPPORTED;
  }
  return udp_open(user, family, port, out, addr);
}
static h2_pal_result_t accept_socket(void *user, int listener, int *out,
                                     h2_pal_net_addr_t *peer, uint32_t ms) {
  fake_t *f = user;
  (void)listener;
  if (f->current == H2_NET_TLS_TCP_ACCEPT_TIMEOUT && !f->prepare_count) {
    f->clock_us += (uint64_t)ms * 1000u;
    return H2_PAL_ERR_TIMEOUT;
  }
  resolve(user, "127.0.0.1", peer);
  return (h2_pal_result_t)open_socket(user, H2_PAL_NET_FAMILY_IPV4, out);
}
static int prepare(void *user, const char *id, h2_net_tls_fixture_mode_t mode,
                   uint16_t callback, uint16_t *port) {
  fake_t *f = user;
  (void)id;
  (void)callback;
  if (f->fail_prepare && f->current == H2_NET_TLS_TLS_WRONG_CA)
    return H2_PAL_ERR_NOT_FOUND;
  f->mode = mode;
  f->written = f->received = 0;
  f->wrapped = 0;
  ++f->prepare_count;
  *port = 123u;
  return H2_PAL_OK;
}
static int verify(void *user, const char *id,
                  h2_net_tls_fixture_proof_t proof) {
  fake_t *f = user;
  (void)id;
  if (f->bad_proof && proof == H2_NET_TLS_PROOF_CERT_REJECTION)
    return H2_PAL_ERR_FORMAT;
  return H2_PAL_OK;
}
static void report(void *user, const h2_net_tls_case_result_t *item) {
  fake_t *f = user;
  (void)item;
  if (f->active_sockets || f->active_resolvers)
    abort();
  f->current = (h2_net_tls_case_t)(f->current + 1);
  f->prepare_count = 0;
}
static h2_pal_net_vtable_t vtable = {.resolve_addr = resolve,
                                     .resolve_start = resolve_start,
                                     .resolve_poll = resolve_poll,
                                     .resolve_close = resolve_close,
                                     .get_host_addr = host_addr,
                                     .udp_open = udp_open,
                                     .udp_open_bound = udp_bound,
                                     .udp_sendto = udp_send,
                                     .udp_recvfrom = udp_recv,
                                     .udp_join_multicast = multicast,
                                     .tcp_open = open_socket,
                                     .tcp_open_bound = open_bound,
                                     .tcp_connect = connect_socket,
                                     .tcp_send = send_bytes,
                                     .tcp_send_timeout = send_timeout,
                                     .tcp_recv = receive_bytes,
                                     .tls_wrap = wrap,
                                     .close = close_socket,
                                     .tcp_listen = listen_socket,
                                     .tcp_accept = accept_socket};

int main(void) {
  const h2_pal_mem_vtable_t memory_vtable = {.alloc = allocate,
                                             .free = release};
  const h2_pal_time_vtable_t time_vtable = {.get_monotonic_us = monotonic,
                                            .sleep_ms = sleep_ms};
  fake_t f = {0};
  h2_pal_mem_api_t memory = {&f, &memory_vtable};
  h2_pal_time_api_t time = {&f, &time_vtable};
  h2_pal_net_api_t net = {&f, &vtable};
  h2_runtime_t runtime = {.mem = &memory, .time = &time, .net = &net};
  h2_net_tls_config_t config = {
      .runtime = &runtime,
      .host = "127.0.0.1",
      .session = "0123456789abcdef0123456789abcdef",
      .root_ca = (const uint8_t *)"root",
      .wrong_ca = (const uint8_t *)"wrong",
      .root_ca_len = 4,
      .wrong_ca_len = 5,
      .dns_host = "ap.e2e.gizclaw.com",
      .dns_expected = {.family = H2_PAL_NET_FAMILY_IPV4,
                       .ip = {150, 5, 151, 236}},
      .server_name = "pal-net-tls.test",
      .prepare = prepare,
      .verify = verify,
      .fixture_user = &f,
      .report = report,
      .report_user = &f};
  for (unsigned fault = 0; fault < 6u; ++fault) {
    memset(&f, 0, sizeof(f));
    f.generic_tls_error = fault == 1u;
    f.bad_proof = fault == 2u;
    f.corrupt = fault == 3u;
    f.fail_allocate = fault == 4u;
    f.fail_prepare = fault == 5u;
    h2_net_tls_result_t result;
    int rc = h2_pal_net_tls_e2e_run(&config, &result);
    if (f.allocations || f.active_sockets || f.active_resolvers)
      return 1;
    if (!fault && (rc != H2_PAL_OK || result.mandatory_passed != 37u ||
                   result.unsupported != 1u || result.not_assessed != 1u))
      return 2;
    if (fault && (rc == H2_PAL_OK || (!result.failed && !result.blocked)))
      return 3;
    if (fault == 1u && result.cases[H2_NET_TLS_TLS_WRONG_CA].passed)
      return 4;
    if (fault == 2u && result.cases[H2_NET_TLS_TLS_WRONG_NAME].passed)
      return 5;
    if (fault == 3u && result.cases[H2_NET_TLS_TLS_REQUIRED].passed)
      return 6;
  }
  /* Every mandatory vtable hole must block all cases before opening a socket.
   * Function pointers are removed by name to avoid representation assumptions.
   */
#define MISSING(slot)                                                          \
  do {                                                                         \
    h2_pal_net_vtable_t saved = vtable;                                        \
    vtable.slot = NULL;                                                        \
    memset(&f, 0, sizeof(f));                                                  \
    h2_net_tls_result_t result;                                                \
    if (h2_pal_net_tls_e2e_run(&config, &result) == H2_PAL_OK ||               \
        result.blocked != H2_NET_TLS_CASE_COUNT || f.next_socket ||            \
        f.allocations)                                                         \
      return 7;                                                                \
    vtable = saved;                                                            \
  } while (0)
  MISSING(resolve_addr);
  MISSING(resolve_start);
  MISSING(resolve_poll);
  MISSING(resolve_close);
  MISSING(get_host_addr);
  MISSING(udp_open);
  MISSING(udp_open_bound);
  MISSING(udp_sendto);
  MISSING(udp_recvfrom);
  MISSING(tcp_open);
  MISSING(tcp_open_bound);
  MISSING(tcp_connect);
  MISSING(tcp_send);
  MISSING(tcp_send_timeout);
  MISSING(tcp_recv);
  MISSING(tls_wrap);
  MISSING(close);
  MISSING(tcp_listen);
  MISSING(tcp_accept);
  return 0;
}
