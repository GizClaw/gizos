#include "h2_desktop_platform.h"
#include "h2_wolfssl.h"
#include "runner.h"
#if defined(__APPLE__)
#include "h2_darwin_platform.h"
#define host_net h2_darwin_net_api
#define host_entropy h2_darwin_entropy
#else
#include "h2_linux_platform.h"
#define host_net h2_linux_net_api
#define host_entropy h2_linux_entropy
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* The local multicast fixture must not also route copies through a VPN/AP.
 * Keep actual PAL socket creation, membership, send and receive under test. */
static int local_multicast_udp_open(void* user, h2_pal_net_family_t family,
                                    uint16_t port, h2_pal_net_socket_t* out,
                                    h2_pal_net_addr_t* address) {
  const h2_pal_net_api_t* net = host_net();
  int rc = net->vtable->udp_open(user, family, port, out, address);
  if (rc != H2_PAL_OK || family != H2_PAL_NET_FAMILY_IPV4) return rc;
  const unsigned char ttl = 0, loop = 1;
  if (setsockopt(*out, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) != 0 ||
      setsockopt(*out, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) !=
          0) {
    h2_pal_net_close(net, *out);
    *out = -1;
    if (address != NULL) memset(address, 0, sizeof(*address));
    return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

static size_t read_ca(const char* path, uint8_t* bytes, size_t capacity) {
  FILE* file = fopen(path, "rb");
  if (!file) return 0u;
  size_t length = fread(bytes, 1u, capacity, file);
  int valid = feof(file) && !ferror(file);
  fclose(file);
  return valid ? length : 0u;
}
int main(int argc, char** argv) {
  if (argc != 8 && argc != 9) return 2;
  uint8_t ca[8192], wrong[8192];
  size_t ca_len = read_ca(argv[4], ca, sizeof(ca));
  size_t wrong_len = read_ca(argv[5], wrong, sizeof(wrong));
  if (!ca_len || !wrong_len) return 2;
  char* end = NULL;
  unsigned long port = strtoul(argv[2], &end, 10);
  if (!end || *end || !port || port > 65535u) return 2;
  h2_pal_net_api_t local_net = *host_net();
  h2_pal_net_vtable_t local_vtable = *local_net.vtable;
  if (argc == 9) {
    local_vtable.udp_open = local_multicast_udp_open;
    local_net.vtable = &local_vtable;
  }
  h2_runtime_t runtime = {.mem = h2_desktop_platform_default_allocator(),
                          .net = &local_net,
                          .time = h2_desktop_platform_time_api()};
  h2_wolfssl_config_t tls = {.mem = *runtime.mem, .entropy = host_entropy};
  if (h2_wolfssl_init(&tls) != H2_PAL_OK) return 2;
  h2_net_tls_fixture_client_t client = {.runtime = &runtime,
                                        .host = argv[1],
                                        .session = argv[3],
                                        .port = (uint16_t)port};
  h2_net_tls_config_t config = {.runtime = &runtime,
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
                                .report = h2_net_tls_report,
                                .multicast_supported = 1,
                                .multicast_host = argc == 9 ? argv[8] : NULL,
                                .case_timeout_ms = 30000u};
  if (h2_net_tls_parse_ipv4(argv[7], &config.dns_expected) != H2_PAL_OK) {
    (void)h2_wolfssl_deinit();
    return 2;
  }
  h2_net_tls_result_t result;
  int rc = h2_pal_net_tls_e2e_run(&config, &result);
  int teardown = h2_wolfssl_deinit();
  h2_net_tls_summary(stdout, &result, rc, teardown);
  return rc == H2_PAL_OK && teardown == H2_PAL_OK ? 0 : 1;
}
