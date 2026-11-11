#include "h2_alipay_tiga_gateway.h"
#include "net/h2_pal_net.h"
#include <stdio.h>
#include <string.h>

h2_pal_result_t h2_alipay_tiga_gateway_probe(h2_runtime_t* rt) {
  const char* host = "iotapi.alipay.com";
  h2_pal_net_addr_list_t addresses;
  int rc =
      h2_pal_net_resolve_all(rt->net, host, H2_PAL_NET_FAMILY_IPV4, &addresses);
  printf("H2_ALIPAY_GATEWAY stage=dns host=%s rc=%d count=%zu\n", host, rc,
         addresses.count);
  if (rc != H2_PAL_OK) return rc;
  if (addresses.count == 0) return H2_PAL_ERR_NOT_FOUND;
  h2_pal_net_addr_t addr = addresses.addrs[0];
  addr.port = 443;
  h2_pal_net_socket_t fd = -1, tls = -1;
  rc = h2_pal_net_tcp_open_bound(rt->net, addr.family, NULL, &fd);
  if (rc == H2_PAL_OK) rc = h2_pal_net_tcp_connect(rt->net, fd, &addr, 10000);
  printf("H2_ALIPAY_GATEWAY stage=tcp ip=%u.%u.%u.%u port=%u rc=%d\n",
         addr.ip[0], addr.ip[1], addr.ip[2], addr.ip[3], addr.port, rc);
  if (rc != H2_PAL_OK) goto done;
  const h2_pal_net_tls_config_t config = {
      .server_name = host,
      .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
  };
  rc = h2_pal_net_tls_wrap(rt->net, fd, &config, 15000, &tls);
  printf("H2_ALIPAY_GATEWAY stage=tls host=%s verify=required rc=%d\n", host,
         rc);
  if (rc != H2_PAL_OK) goto done;
  static const char request[] =
      "HEAD /mgw.htm HTTP/1.1\r\nHost: iotapi.alipay.com\r\n"
      "Connection: close\r\n\r\n";
  size_t sent = 0;
  while (sent < sizeof(request) - 1) {
    int n = h2_pal_net_tcp_send_timeout(rt->net, tls,
                                        (const uint8_t*)request + sent,
                                        sizeof(request) - 1 - sent, 5000);
    if (n <= 0) {
      rc = n < 0 ? n : H2_PAL_ERR_CLOSED;
      goto done;
    }
    sent += (size_t)n;
  }
  /* Retain only the status code; no headers or body leave this function. */
  char response[65] = {0};
  size_t used = 0;
  int status = 0;
  while (used < sizeof(response) - 1 && strchr(response, '\n') == NULL) {
    int n = h2_pal_net_tcp_recv(rt->net, tls, (uint8_t*)response + used,
                                sizeof(response) - 1 - used, 5000);
    if (n <= 0) {
      rc = n < 0 ? n : H2_PAL_ERR_CLOSED;
      goto done;
    }
    used += (size_t)n;
  }
  rc = sscanf(response, "HTTP/%*u.%*u %d", &status) == 1 && status >= 100 &&
               status <= 599
           ? H2_PAL_OK
           : H2_PAL_ERR_IO;
  printf(
      "H2_ALIPAY_GATEWAY stage=http method=HEAD path=/mgw.htm "
      "status=%d rc=%d\n",
      status, rc);
done:
  /* ESP PAL wraps this same descriptor; close exactly once. */
  h2_pal_net_close(rt->net, fd);
  printf("H2_ALIPAY_GATEWAY complete=true rc=%d\n", rc);
  fflush(stdout);
  return rc;
}
