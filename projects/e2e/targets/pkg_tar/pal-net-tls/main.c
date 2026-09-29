#include "h2/pal/h2_pal_unsupported.h"
#include "h2_web_app_host.h"
#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdio.h>

const h2_web_board_t h2_web_board = {.display_width = 1, .display_height = 1};
/* clang-format off */
EM_JS(int, worker, (), { return ENVIRONMENT_IS_PTHREAD && typeof window === 'undefined' ? 1 : 0; });
/* clang-format on */
static unsigned calls, errors;
static void observed(const char *slot, int rc) {
  ++calls;
  if (rc != H2_PAL_ERR_UNSUPPORTED)
    ++errors;
  printf("H2_PAL_NET_TLS_CAPABILITY "
         "{\"slot\":\"%s\",\"status\":\"UNSUPPORTED\",\"result\":%d}\n",
         slot, rc);
}
static void observed_void(const char *slot) {
  ++calls;
  printf("H2_PAL_NET_TLS_CAPABILITY "
         "{\"slot\":\"%s\",\"status\":\"UNSUPPORTED_NOOP\",\"result\":null,"
         "\"owned_handle\":false}\n",
         slot);
}
static h2_pal_result_t boundary(h2_web_app_host_t *host, h2_runtime_t *runtime,
                                void *user) {
  (void)host;
  (void)user;
  if (!worker() || emscripten_is_main_runtime_thread() || !runtime->net ||
      !runtime->net->vtable || runtime->net != h2_pal_unsupported_net_api())
    return H2_PAL_ERR_INVALID_STATE;
  const h2_pal_net_vtable_t *v = runtime->net->vtable;
  void *context = runtime->net->user;
  int socket = -1;
  uint8_t byte = 0u;
  h2_pal_net_addr_t addr = {.family = H2_PAL_NET_FAMILY_IPV4,
                            .port = 1,
                            .ip = {127, 0, 0, 1}},
                    out = {0};
  h2_pal_net_bind_t bind = {.type = H2_PAL_NET_BIND_SOURCE_ADDR,
                            .source_addr = addr};
  h2_pal_net_resolver_t *resolver = NULL;
  h2_pal_net_tls_config_t tls = {.server_name = "pal-net-tls.test",
                                 .verify = H2_PAL_NET_TLS_VERIFY_REQUIRED};
  h2_pal_net_icmp_echo_result_t echo = {0};
  observed("resolve_addr", v->resolve_addr(context, "127.0.0.1", &out));
  observed("resolve_start", v->resolve_start(context, "127.0.0.1", &resolver));
  observed("resolve_poll", v->resolve_poll(context, resolver, &out, 1u));
  v->resolve_close(context, resolver);
  observed_void("resolve_close"); /* no owned handle exists */
  observed("get_host_addr", v->get_host_addr(context, NULL, &out));
  observed("udp_open", v->udp_open(context, addr.family, 0u, &socket, &out));
  observed("udp_open_bound",
           v->udp_open_bound(context, addr.family, 0u, &bind, &socket, &out));
  observed("udp_sendto", v->udp_sendto(context, 0, &addr, &byte, 1u));
  observed("udp_recvfrom", v->udp_recvfrom(context, 0, &out, &byte, 1u, 1u));
  observed("udp_join_multicast", v->udp_join_multicast(context, 0, &addr));
  observed("tcp_open", v->tcp_open(context, addr.family, &socket));
  observed("tcp_open_bound",
           v->tcp_open_bound(context, addr.family, &bind, &socket));
  observed("tcp_connect", v->tcp_connect(context, 0, &addr, 1u));
  observed("tcp_send", v->tcp_send(context, 0, &byte, 1u));
  observed("tcp_send_timeout", v->tcp_send_timeout(context, 0, &byte, 1u, 1u));
  observed("tcp_recv", v->tcp_recv(context, 0, &byte, 1u, 1u));
  observed("tls_wrap", v->tls_wrap(context, 0, &tls, 1u, &socket));
  observed("icmp_echo", v->icmp_echo(context, &addr, NULL, 1u, &echo));
  v->close(context, 0);
  observed_void("close"); /* canonical no-op, not a supported close */
  observed("tcp_listen",
           v->tcp_listen(context, addr.family, 0u, NULL, &socket, &out));
  observed("tcp_accept", v->tcp_accept(context, 0, &socket, &out, 1u));
  printf("H2_PAL_NET_TLS_WEB_BOUNDARY "
         "{\"platform\":\"wasm\",\"worker\":%d,\"main_runtime_thread\":%d,"
         "\"operations\":%u,\"unexpected_results\":%u,\"core_qualified\":false,"
         "\"full_net_qualified\":false,\"reason\":\"AppHost raw Net uses "
         "canonical unsupported API; browser HOST exposes Fetch/WebRTC\"}\n",
         worker(), emscripten_is_main_runtime_thread(), calls, errors);
  return errors || calls != 21u ? H2_PAL_ERR_IO : H2_PAL_OK;
}
int main(void) {
  const h2_web_app_host_config_t config = {
      .name = "pal-net-tls-boundary", .display_width = 1, .display_height = 1};
  return h2_web_app_host_run(&config, boundary, NULL);
}
