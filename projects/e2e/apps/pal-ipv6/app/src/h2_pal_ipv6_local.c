#include "h2_pal_ipv6_local.h"
#include <stdio.h>
#include <string.h>

struct h2_pal_ipv6_cleanup {
  h2_runtime_t runtime;
  h2_pal_task_t *task;
  int listener;
  int rc;
  int cleanup_error;
  char session[33];
};

static void local_http_serve(void *user) {
  h2_pal_ipv6_cleanup_t *peer = user;
  const h2_pal_net_api_t *net = peer->runtime.net;
  int client = -1;
  h2_pal_net_addr_t remote;
  peer->rc = h2_pal_net_tcp_accept(net, peer->listener, &client, &remote, 5000u);
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
  char expected[64];
  snprintf(expected, sizeof(expected), "GET /%s HTTP/1.1\r\n", peer->session);
  if (peer->rc == H2_PAL_OK &&
      strncmp(request, expected, strlen(expected)) != 0)
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
  if (client >= 0)
    h2_pal_net_close(net, client);
}

int h2_pal_ipv6_local_cleanup(h2_pal_ipv6_cleanup_t **pending) {
  if (!pending)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_ipv6_cleanup_t *peer = *pending;
  if (!peer)
    return H2_PAL_OK;
  if (peer->task) {
    int rc = h2_pal_task_join(peer->runtime.task, peer->task);
    if (rc != H2_PAL_OK) {
      peer->cleanup_error = rc;
      return rc;
    }
    peer->task = NULL;
  }
  if (peer->listener >= 0)
    h2_pal_net_close(peer->runtime.net, peer->listener);
  const h2_pal_mem_api_t *mem = peer->runtime.mem;
  h2_pal_mem_free(mem, peer);
  *pending = NULL;
  return H2_PAL_OK;
}

int h2_pal_ipv6_local_cleanup_error(const h2_pal_ipv6_cleanup_t *pending) {
  return pending ? pending->cleanup_error : H2_PAL_OK;
}

int h2_pal_ipv6_local_http(const h2_runtime_t *runtime, const char *session,
                           h2_pal_ipv6_http_call_t call, void *user,
                           h2_pal_ipv6_cleanup_t **pending) {
  if (!runtime || !session || strlen(session) != 32u || !call || !pending)
    return H2_PAL_ERR_INVALID_ARG;
  if (*pending)
    return H2_PAL_ERR_INVALID_STATE;
  h2_pal_ipv6_cleanup_t *peer = h2_pal_mem_alloc(runtime->mem, sizeof(*peer));
  if (!peer)
    return H2_PAL_ERR_NO_MEMORY;
  memset(peer, 0, sizeof(*peer));
  peer->runtime = *runtime;
  peer->listener = -1;
  memcpy(peer->session, session, sizeof(peer->session));
  h2_pal_net_bind_t bind = {
      .type = H2_PAL_NET_BIND_SOURCE_ADDR,
      .source_addr = {.family = H2_PAL_NET_FAMILY_IPV4, .ip = {127, 0, 0, 1}}};
  h2_pal_net_addr_t address;
  int rc = h2_pal_net_tcp_listen(runtime->net, H2_PAL_NET_FAMILY_IPV4, 0u,
                                 &bind, &peer->listener, &address);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(runtime->mem, peer);
    return rc;
  }
  h2_pal_task_options_t options = {.name = "pal-ipv6/e2e/http-peer"};
  rc = h2_pal_task_start(runtime->task, &options, local_http_serve, peer,
                         &peer->task);
  if (rc == H2_PAL_OK) {
    char url[128];
    snprintf(url, sizeof(url), "http://localhost:%u/%s", (unsigned)address.port,
             peer->session);
    rc = call(user, url);
    int joined = h2_pal_task_join(runtime->task, peer->task);
    if (joined != H2_PAL_OK) {
      peer->cleanup_error = joined;
      *pending = peer;
      return rc != H2_PAL_OK ? rc : joined;
    }
    peer->task = NULL;
    if (rc == H2_PAL_OK)
      rc = peer->rc;
  }
  *pending = peer;
  (void)h2_pal_ipv6_local_cleanup(pending);
  return rc;
}

static int connect_local(const h2_pal_net_api_t *net,
                         h2_pal_net_family_t family, uint16_t port,
                         int listener) {
  int socket = -1, accepted = -1;
  h2_pal_net_addr_t address = {.family = family, .port = port};
  if (family == H2_PAL_NET_FAMILY_IPV4) {
    address.ip[0] = 127u;
    address.ip[3] = 1u;
  } else {
    address.ip[15] = 1u;
  }
  int rc = h2_pal_net_tcp_open_bound(net, family, NULL, &socket);
  if (rc == H2_PAL_OK)
    rc = h2_pal_net_tcp_connect(net, socket, &address, 1000u);
  if (rc == H2_PAL_OK && listener >= 0) {
    h2_pal_net_addr_t remote;
    rc = h2_pal_net_tcp_accept(net, listener, &accepted, &remote, 1000u);
    if (rc == H2_PAL_OK && remote.family != family)
      rc = H2_PAL_ERR_FORMAT;
  }
  if (accepted >= 0)
    h2_pal_net_close(net, accepted);
  if (socket >= 0)
    h2_pal_net_close(net, socket);
  return rc;
}

int h2_pal_ipv6_local_isolation(const h2_pal_net_api_t *net) {
  int listener6 = -1, listener4 = -1;
  h2_pal_net_addr_t address;
  int rc = h2_pal_net_tcp_listen(net, H2_PAL_NET_FAMILY_IPV6, 0u, NULL,
                                 &listener6, &address);
  if (rc != H2_PAL_OK)
    return rc;
  const uint16_t port = address.port;
  rc = connect_local(net, H2_PAL_NET_FAMILY_IPV6, port, listener6);
  if (rc == H2_PAL_OK)
    rc = h2_pal_net_tcp_listen(net, H2_PAL_NET_FAMILY_IPV4, port, NULL,
                               &listener4, &address);
  if (rc == H2_PAL_OK)
    rc = connect_local(net, H2_PAL_NET_FAMILY_IPV4, port, listener4);
  if (listener4 >= 0) {
    h2_pal_net_close(net, listener4);
    listener4 = -1;
  }
  if (rc == H2_PAL_OK) {
    int refused = connect_local(net, H2_PAL_NET_FAMILY_IPV4, port, -1);
    rc = refused == H2_PAL_ERR_IO ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
  }
  h2_pal_net_close(net, listener6);
  return rc;
}
