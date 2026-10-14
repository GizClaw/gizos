#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
/* Compile the complete production transport, including its static handshake. */
#include "libs/h2loader_host/src/h2_h2loader_host_serial.c"

static uint64_t now;
static uint8_t wire[512];
static size_t wire_size, position, chunk_limit;
static char received[512];
static size_t logged;
static h2_pal_result_t monotonic(void *user, uint64_t *out) {
  (void)user;
  *out = now;
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_methods = {.get_monotonic_ms =
                                                      monotonic};
static const h2_pal_time_api_t clock_api = {.vtable = &time_methods};
static h2_pal_result_t read_wire(void *user, void *out, size_t capacity,
                                 size_t *length, uint32_t wait) {
  (void)user;
  now += wait;
  size_t n = wire_size - position;
  if (n > capacity)
    n = capacity;
  if (n > chunk_limit)
    n = chunk_limit;
  memcpy(out, wire + position, n);
  position += n;
  *length = n;
  return n ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}
static h2_pal_result_t write_wire(void *user, const void *data, size_t n,
                                  size_t *out, uint32_t wait) {
  (void)user;
  (void)data;
  (void)wait;
  *out = n;
  return H2_PAL_OK;
}
static h2_pal_result_t flush_wire(void *user) {
  (void)user;
  return H2_PAL_OK;
}
static const h2_pal_uart_io_stream_vtable_t uart_methods = {
    .read = read_wire, .write = write_wire, .flush = flush_wire};
static const h2_pal_uart_io_stream_api_t uart_api = {.vtable = &uart_methods};
static h2_pal_result_t sink(void *user, const uint8_t *data, size_t n) {
  (void)user;
  assert(logged + n < sizeof(received));
  memcpy(received + logged, data, n);
  logged += n;
  return H2_PAL_OK;
}
int main(void) {
  const char fresh[] =
      "H2_STORAGE_BOOT contract=2 version=wire-test phase=1 nonce=17\n";
  for (int ack = 0; ack < 2; ++ack) {
    for (size_t chunk = 1; chunk <= 128; chunk *= 2) {
      now = 0;
      position = 0;
      logged = 0;
      chunk_limit = chunk;
      memcpy(wire, fresh, sizeof(fresh) - 1);
      wire_size = sizeof(fresh) - 1;
      if (ack) {
        const uint8_t conv[] = {7, 0, 0, 0};
        h2_iostreamikcp_frame_t frame = {
            .flags = H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK,
            .conv = 7,
            .payload = conv,
            .payload_len = sizeof(conv)};
        size_t encoded = 0;
        assert(h2_iostreamikcp_frame_encode(&frame, wire + wire_size,
                                            sizeof(wire) - wire_size,
                                            &encoded) == H2_PAL_OK);
        wire_size += encoded;
      }
      h2_h2loader_host_serial_connection_t connection = {
          .time = &clock_api, .uart = &uart_api, .on_log = sink};
      int rc = serial_session_control(
          &connection, 7, H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN, 2000);
      assert(rc == (ack ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT));
      assert(logged == sizeof(fresh) - 1 && !memcmp(received, fresh, logged));
    }
  }
  return 0;
}
