#include "h2_desktop_platform.h"
#include "h2_pal_ipv6_runner.h"
#include "h2_peer.h"
#include "h2_sctp.h"
#include "h2_wolfssl.h"
#include "projects/e2e/libs/pal-net-tls-runner/runner.h"
#if defined(__APPLE__)
#include "h2_darwin_platform.h"
#define host_net h2_darwin_net_api
#define host_entropy h2_darwin_entropy
#define host_platform "macos"
#else
#include "h2_linux_platform.h"
#define host_net h2_linux_net_api
#define host_entropy h2_linux_entropy
#define host_platform "linux"
#endif
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>

/* Every peer in this Desktop fixture is on loopback. Select that actual OS
 * interface for ICE host candidates instead of depending on an active NIC
 * having a signalable, non-link-local IPv6 address. Socket I/O stays real. */
static int loopback_host_family(void *user, const char *prefix,
    h2_pal_net_family_t family, h2_pal_net_addr_t *out) {
  (void)user;
  if (prefix && prefix[0])
    return h2_pal_net_get_host_addr_family(host_net(), prefix, family, out);
  h2_pal_net_addr_t assigned;
  int rc = h2_pal_net_get_host_addr_family(host_net(),
#if defined(__APPLE__)
      "lo0",
#else
      "lo",
#endif
      family, &assigned);
  if (rc != H2_PAL_OK)
    return rc;
  /* An UP loopback interface may also have a scoped fe80 address. Obtain its
   * unscoped localhost answer from the owned resolver; do not serialize a
   * host-local interface scope or invent a candidate address. */
  h2_pal_net_addr_list_t addresses;
  rc = h2_pal_net_resolve_all(host_net(), "localhost", family, &addresses);
  if (rc != H2_PAL_OK || addresses.count == 0u)
    return rc != H2_PAL_OK ? rc : H2_PAL_ERR_NOT_FOUND;
  *out = addresses.addrs[0];
  char line[100];
  snprintf(line, sizeof(line),
           "H2_PAL_IPV6_HOST_SELECTION family=%d assigned_scope=%u selected_scope=%u",
           (int)family, assigned.scope_id, out->scope_id);
  h2_pal_log_write(h2_desktop_platform_log_api(), H2_PAL_LOG_INFO, "pal-ipv6", line);
  return H2_PAL_OK;
}

typedef struct desktop_owner {
  h2_runtime_t runtime;
  h2_pal_net_vtable_t ice_vtable;
  h2_pal_net_api_t ice_net;
  h2_sctp_t *sctp;
  h2_peer_t *peer;
  h2_pal_ipv6_result_t result;
  uint8_t ca[8192], wrong[8192];
} desktop_owner_t;
/* Failed cleanup leaves this heap owner reachable until process termination. */
static desktop_owner_t *retained_owner;

static size_t read_ca(const char *path, uint8_t *bytes, size_t capacity) {
  FILE *file = fopen(path, "rb");
  if (!file)
    return 0u;
  size_t length = fread(bytes, 1u, capacity, file);
  int valid = feof(file) && !ferror(file);
  fclose(file);
  return valid ? length : 0u;
}
int main(int argc, char **argv) {
  if (argc != 14)
    return 2;
  desktop_owner_t *owner = calloc(1u, sizeof(*owner));
  if (!owner)
    return 2;
  uint8_t *ca = owner->ca, *wrong = owner->wrong;
  size_t ca_len = read_ca(argv[4], ca, sizeof(owner->ca));
  size_t wrong_len = read_ca(argv[5], wrong, sizeof(owner->wrong));
  if (!ca_len || !wrong_len)
    return 2;
  char *end = NULL;
  unsigned long port = strtoul(argv[2], &end, 10);
  if (!end || *end || !port || port > 65535u)
    return 2;
  owner->runtime = (h2_runtime_t){.mem = h2_desktop_platform_default_allocator(),
                          .log = h2_desktop_platform_log_api(),
                          .net = host_net(),
                          .time = h2_desktop_platform_time_api(),
                          .task = h2_desktop_platform_task_api()};
  h2_runtime_t *runtime = &owner->runtime;
  h2_wolfssl_config_t tls = {.mem = *runtime->mem, .entropy = host_entropy};
  if (h2_wolfssl_init(&tls) != H2_PAL_OK)
    return 2;
  runtime->crypto = h2_wolfssl_crypto_api();
  owner->ice_vtable = *runtime->net->vtable;
  owner->ice_vtable.get_host_addr_family = loopback_host_family;
  owner->ice_net = (h2_pal_net_api_t){runtime->net->user, &owner->ice_vtable};
  h2_sctp_t *sctp = NULL;
  const h2_sctp_config_t sctp_config = {.mem = runtime->mem, .crypto = runtime->crypto};
  if (h2_sctp_create(&sctp_config, &sctp) != H2_PAL_OK) {
    (void)h2_wolfssl_deinit();
    return 2;
  }
  h2_peer_t *peer = NULL;
  const h2_peer_config_t peer_config = {.mem = runtime->mem, .log = runtime->log,
      .net = &owner->ice_net, .queue = h2_desktop_platform_queue_api(),
      .sync = h2_desktop_platform_sync_api(), .task = h2_desktop_platform_task_api(),
      .time = runtime->time, .crypto = runtime->crypto,
      .dtls = h2_wolfssl_dtls_api(), .sctp = h2_sctp_api(sctp)};
  if (h2_peer_create(&peer_config, &peer) != H2_PAL_OK) {
    (void)h2_sctp_destroy(&sctp);
    (void)h2_wolfssl_deinit();
    return 2;
  }
  runtime->webrtc = h2_peer_webrtc_api(peer);
  h2_net_tls_fixture_client_t client = {.runtime = runtime,
                                        .host = argv[1],
                                        .session = argv[3],
                                        .port = (uint16_t)port};
  h2_pal_ipv6_config_t config = {
      .netif = {.type = H2_PAL_NETIF_REF_ID,
                .id = if_nametoindex(
#if defined(__APPLE__)
                    "lo0"
#else
                    "lo"
#endif
                    )},
      .local_ipv6 = {.family = H2_PAL_NET_FAMILY_IPV6,
                     .ip = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
      .dtls = h2_wolfssl_dtls_api(),
      .offer_url = argv[11],
      .stun_url = argv[12],
      .http_url = argv[8],
      .fallback_url = argv[9],
      .mqtt_host = argv[1],
      .mqtt_port = (uint16_t)strtoul(argv[10], NULL, 10),
      .transport = {.runtime = runtime,
                    .host = argv[1],
                    .session = argv[3],
                    .root_ca = ca,
                    .root_ca_len = ca_len,
                    .wrong_ca = wrong,
                    .wrong_ca_len = wrong_len,
                    .dns_host = argv[6],
                    .server_name = "pal-net-tls.test",
                    .prepare = h2_net_tls_fixture_prepare,
                    .verify = h2_net_tls_fixture_verify,
                    .fixture_user = &client,
                    .report = h2_ipv6_report,
                    .multicast_supported = 1,
                    .case_timeout_ms = 30000u,
                    .family = H2_PAL_NET_FAMILY_IPV6}};
  if (h2_ipv6_parse_address(runtime->net, argv[7],
                            &config.transport.dns_expected) != H2_PAL_OK) {
    h2_peer_destroy(&peer);
    (void)h2_sctp_destroy(&sctp);
    (void)h2_wolfssl_deinit();
    return 2;
  }
  if (h2_ipv6_parse_address(runtime->net, argv[1], &config.dns_server) !=
      H2_PAL_OK) {
    h2_peer_destroy(&peer);
    (void)h2_sctp_destroy(&sctp);
    (void)h2_wolfssl_deinit();
    return 2;
  }
  config.dns_answer = config.dns_server;
  config.dns_server.port = (uint16_t)strtoul(argv[13], NULL, 10);
  owner->peer = peer;
  owner->sctp = sctp;
  int rc = h2_pal_ipv6_e2e_run(&config, &owner->result);
  int local_cleanup = h2_pal_ipv6_e2e_cleanup(&owner->result);
  if (local_cleanup != H2_PAL_OK) {
    retained_owner = owner;
    h2_ipv6_write_report(NULL, host_platform, &owner->result, rc, local_cleanup);
    return 1;
  }
  h2_peer_destroy(&peer);
  int teardown = h2_sctp_destroy(&sctp);
  int crypto_teardown = h2_wolfssl_deinit();
  if (teardown == H2_PAL_OK)
    teardown = crypto_teardown;
  h2_ipv6_write_report(NULL, host_platform, &owner->result, rc, teardown);
  if (teardown == H2_PAL_OK)
    free(owner);
  else
    retained_owner = owner;
  return rc == H2_PAL_OK && teardown == H2_PAL_OK ? 0 : 1;
}
