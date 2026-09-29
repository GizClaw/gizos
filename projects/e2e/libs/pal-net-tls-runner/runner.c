#include "runner.h"
#include <stdio.h>
#include <string.h>

static uint64_t monotonic(const h2_runtime_t *runtime) {
  uint64_t us = 0u;
  return h2_pal_time_get_monotonic_us(runtime->time, &us) == H2_PAL_OK
             ? us / 1000u
             : UINT64_MAX;
}
static int control(h2_net_tls_fixture_client_t *client, const char *request,
                   uint16_t *port) {
  h2_pal_net_addr_t addr = {0};
  int rc = h2_pal_net_resolve_addr(client->runtime->net, client->host, &addr);
  if (rc != H2_PAL_OK)
    return rc;
  addr.port = client->port;
  int socket = -1;
  rc = h2_pal_net_tcp_open_bound(client->runtime->net, addr.family, NULL,
                                 &socket);
  if (rc != H2_PAL_OK)
    return rc;
  uint64_t deadline = monotonic(client->runtime) + 5000u;
  do {
    rc = h2_pal_net_tcp_connect(client->runtime->net, socket, &addr, 50u);
  } while ((rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) &&
           monotonic(client->runtime) < deadline);
  size_t sent = 0u, len = strlen(request);
  while (rc == H2_PAL_OK && sent < len &&
         monotonic(client->runtime) < deadline) {
    int bytes = h2_pal_net_tcp_send_timeout(client->runtime->net, socket,
                                            (const uint8_t *)request + sent,
                                            len - sent, 50u);
    if (bytes > 0 && (size_t)bytes <= len - sent)
      sent += (size_t)bytes;
    else if (bytes != H2_PAL_ERR_TIMEOUT && bytes != H2_PAL_ERR_WOULD_BLOCK)
      rc = bytes < 0 ? bytes : H2_PAL_ERR_FORMAT;
  }
  if (sent < len && rc == H2_PAL_OK)
    rc = H2_PAL_ERR_TIMEOUT;
  char reply[64] = {0};
  size_t got = 0u;
  while (rc == H2_PAL_OK && got < sizeof(reply) - 1u &&
         monotonic(client->runtime) < deadline) {
    int bytes = h2_pal_net_tcp_recv(client->runtime->net, socket,
                                    (uint8_t *)reply + got,
                                    sizeof(reply) - 1u - got, 50u);
    if (bytes > 0 && (size_t)bytes <= sizeof(reply) - 1u - got) {
      got += (size_t)bytes;
      if (memchr(reply, '\n', got))
        break;
    } else if (bytes != H2_PAL_ERR_TIMEOUT && bytes != H2_PAL_ERR_WOULD_BLOCK)
      rc = bytes < 0 ? bytes : H2_PAL_ERR_FORMAT;
  }
  h2_pal_net_close(client->runtime->net, socket);
  unsigned value = 0u;
  char newline = 0;
  if (rc == H2_PAL_OK && (sscanf(reply, "OK %u%c", &value, &newline) != 2 ||
                          newline != '\n' || value > 65535u))
    rc = H2_PAL_ERR_FORMAT;
  if (rc == H2_PAL_OK && port)
    *port = (uint16_t)value;
  return rc;
}
int h2_net_tls_fixture_prepare(void *user, const char *id,
                               h2_net_tls_fixture_mode_t mode,
                               uint16_t callback, uint16_t *port) {
  h2_net_tls_fixture_client_t *client = user;
  char request[160];
  int size = snprintf(request, sizeof(request), "ARM %s %.16s %s %u %u\n",
                      client->session,
                      client->run_id ? client->run_id : client->session, id,
                      (unsigned)mode, callback);
  if (size <= 0 || (size_t)size >= sizeof(request))
    return H2_PAL_ERR_INVALID_ARG;
  return control(client, request, port);
}
int h2_net_tls_fixture_verify(void *user, const char *id,
                              h2_net_tls_fixture_proof_t proof) {
  h2_net_tls_fixture_client_t *client = user;
  char request[160];
  int size = snprintf(
      request, sizeof(request), "PROOF %s %.16s %s %u\n", client->session,
      client->run_id ? client->run_id : client->session, id, (unsigned)proof);
  if (size <= 0 || (size_t)size >= sizeof(request))
    return H2_PAL_ERR_INVALID_ARG;
  return control(client, request, NULL);
}
