/* Compile the complete production unit; only SDK and join boundaries are
 * replaced. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#define h2_loader_app_client_stop_return_console test_stop_return_console
#define h2_iostreamikcp_close test_close_stream
#include "projects/h2loader/native_component_src/esp-idf6.x/h2_h2loader_runtime/src/h2_esp_h2loader_iostreamikcp.c"
static int join_result, joins, closes;
static h2_loader_app_client_t client;
int test_stop_return_console(h2_loader_app_client_t *value) {
  assert(value == &client);
  assert(
      h2_atomic_bool_load(&s_app_transport.stop_requested, H2_ATOMIC_ACQUIRE));
  ++joins;
  return join_result;
}
void test_close_stream(h2_iostreamikcp_t *stream) {
  assert(stream == (h2_iostreamikcp_t *)&client);
  assert(joins > 0 && join_result == H2_PAL_OK);
  ++closes;
}
const h2_pal_uart_io_stream_api_t *h2_esp_platform_uart_io_stream_api(void) {
  return NULL;
}
int main(void) {
  assert(h2_esp_h2loader_app_iostreamikcp_stop() == H2_PAL_OK && joins == 0);
  s_app_transport_started = 1;
  s_app_transport.client = &client;
  s_app_transport.transport.stream = (h2_iostreamikcp_t *)&client;
  s_app_transport.transport.pending_conv = 577;
  assert(h2_atomic_bool_init(&s_app_transport.stop_requested, false) ==
         H2_ATOMIC_OK);
  join_result = H2_PAL_ERR_TIMEOUT;
  assert(h2_esp_h2loader_app_iostreamikcp_stop() == H2_PAL_ERR_TIMEOUT);
  assert(s_app_transport_started && s_app_transport.client == &client &&
         closes == 0);
  assert(s_app_transport.transport.pending_conv == 577 &&
         s_app_transport.stop_requested.storage);
  assert(app_read_byte(&s_app_transport, 10) == EOF);
  assert(app_write(&s_app_transport, "packet", 6) == H2_PAL_ERR_CLOSED);
  join_result = H2_PAL_OK;
  assert(h2_esp_h2loader_app_iostreamikcp_stop() == H2_PAL_OK);
  assert(joins == 2 && closes == 1 && !s_app_transport_started);
  assert(!s_app_transport.client && !s_app_transport.stop_requested.storage);
  assert(!s_app_transport.transport.stream &&
         !s_app_transport.transport.pending_conv);
  assert(h2_esp_h2loader_app_iostreamikcp_stop() == H2_PAL_OK && joins == 2 &&
         closes == 1);
  return 0;
}
