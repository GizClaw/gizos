#include "h2_device_serial.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint64_t s_now;
static uint8_t s_input[4096];
static size_t s_input_len;
static int s_read_error;
static unsigned s_control_writes, s_initializations;
static int s_initialize_error;
static uint32_t s_last_ack;

static void* alloc_mem(void* user, size_t len) {
  (void)user;
  return malloc(len);
}
static void* realloc_mem(void* user, void* p, size_t len) {
  (void)user;
  return realloc(p, len);
}
static void free_mem(void* user, void* p) {
  (void)user;
  free(p);
}
static const h2_pal_mem_vtable_t s_mem_vtable = {
    .alloc = alloc_mem,
    .realloc = realloc_mem,
    .free = free_mem,
};
static const h2_pal_mem_api_t s_mem = {.vtable = &s_mem_vtable};
static h2_pal_result_t get_time(void* user, uint64_t* out) {
  (void)user;
  *out = s_now;
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t s_time_vtable = {.get_monotonic_ms =
                                                       get_time};
static const h2_pal_time_api_t s_time = {.vtable = &s_time_vtable};

static h2_pal_result_t physical_read(void* user, void* data, size_t len,
                                     size_t* out, uint32_t timeout) {
  assert(user == &s_input_len);
  *out = 0u;
  if (s_read_error) return s_read_error;
  if (s_input_len != 0u) {
    *out = s_input_len < len ? s_input_len : len;
    memcpy(data, s_input, *out);
    memmove(s_input, s_input + *out, s_input_len - *out);
    s_input_len -= *out;
    return H2_PAL_OK;
  }
  s_now += timeout;
  return H2_PAL_ERR_TIMEOUT;
}
static h2_pal_result_t observe_frame(void* user,
                                     const h2_iostreamikcp_frame_t* frame) {
  (void)user;
  if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK) {
    ++s_control_writes;
    s_last_ack = frame->conv;
  }
  return H2_PAL_OK;
}
static h2_pal_result_t physical_write(void* user, const void* data, size_t len,
                                      size_t* out, uint32_t timeout) {
  assert(user == &s_input_len && timeout != 0u);
  h2_iostreamikcp_filter_t filter;
  h2_iostreamikcp_filter_init(&filter);
  assert(h2_iostreamikcp_filter_input(&filter, data, len, observe_frame,
                                      NULL) == H2_PAL_OK);
  *out = len;
  return H2_PAL_OK;
}
static h2_pal_result_t physical_flush(void* user) {
  assert(user == &s_input_len);
  return H2_PAL_OK;
}
static const h2_iostreamikcp_io_t s_physical = {
    .user = &s_input_len,
    .read = physical_read,
    .write = physical_write,
    .flush = physical_flush,
};
static h2_pal_result_t initialize(void* user) {
  assert(user == &s_initializations);
  ++s_initializations;
  return s_initialize_error;
}
static void control(uint8_t flags, uint32_t conv) {
  uint8_t payload[4] = {(uint8_t)conv, (uint8_t)(conv >> 8),
                        (uint8_t)(conv >> 16), (uint8_t)(conv >> 24)};
  const h2_iostreamikcp_frame_t frame = {
      .flags = flags,
      .conv = conv,
      .payload = payload,
      .payload_len = 4u,
  };
  assert(s_input_len == 0u);
  assert(h2_iostreamikcp_frame_encode(&frame, s_input, sizeof(s_input),
                                      &s_input_len) == H2_PAL_OK);
}
static void open_session(h2_device_serial_t* transport, uint32_t conv) {
  control(H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN, conv);
  assert(h2_device_serial_poll_session(transport, 50u) == H2_PAL_OK);
  assert(transport->pending_conv == conv);
  assert(h2_device_serial_activate_pending(transport) == H2_PAL_OK);
  assert(transport->conv == conv && s_last_ack == conv);
}

static void test_endpoint(void) {
  h2_device_serial_endpoint_t no_default = {0};
  assert(h2_device_serial_endpoint_start(&no_default, NULL, NULL, NULL) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(h2_device_serial_endpoint_configure(&no_default, &s_physical) ==
         H2_PAL_ERR_INVALID_STATE);
  h2_device_serial_endpoint_t endpoint = {0};
  assert(h2_device_serial_endpoint_configure(&endpoint, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  h2_iostreamikcp_io_t io = s_physical;
  assert(h2_device_serial_endpoint_configure(&endpoint, &io) == H2_PAL_OK);
  io.read = NULL;
  io.user = NULL;
  assert(endpoint.io.read == physical_read && endpoint.io.user == &s_input_len);
  assert(h2_device_serial_endpoint_configure(&endpoint, &s_physical) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_device_serial_endpoint_start(&endpoint, NULL, initialize,
                                         &s_initializations) == H2_PAL_OK);
  assert(s_initializations == 0u);
  endpoint = (h2_device_serial_endpoint_t){0};
  s_initialize_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_device_serial_endpoint_start(&endpoint, &s_physical, initialize,
                                         &s_initializations) ==
         H2_PAL_ERR_TIMEOUT);
  assert(h2_device_serial_endpoint_configure(&endpoint, &s_physical) ==
         H2_PAL_ERR_INVALID_STATE);
  s_initialize_error = H2_PAL_OK;
  assert(h2_device_serial_endpoint_start(&endpoint, &s_physical, initialize,
                                         &s_initializations) == H2_PAL_OK);
  assert(h2_device_serial_endpoint_start(&endpoint, &s_physical, initialize,
                                         &s_initializations) == H2_PAL_OK);
  assert(s_initializations == 2u && endpoint.io.user == &s_input_len);
}

static void test_sessions(void) {
  h2_device_serial_t transport;
  assert(h2_device_serial_init(&transport, &s_mem, &s_time, &s_physical) ==
         H2_PAL_OK);
  open_session(&transport, 11u);
  h2_iostreamikcp_t* first = transport.stream;
  unsigned before = s_control_writes;
  control(H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN, 11u);
  assert(h2_device_serial_poll_session(&transport, 50u) == H2_PAL_OK);
  assert(transport.stream == first && !transport.pending_conv);
  assert(s_control_writes == before + 1u && s_last_ack == 11u);
  control(H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN, 22u);
  assert(h2_device_serial_poll_session(&transport, 50u) == H2_PAL_OK);
  assert(transport.replacement_pending && transport.pending_conv == 22u);
  h2_command_io_api_t command = h2_device_serial_io(&transport);
  char value = 0;
  size_t count = 999u;
  assert(command.vtable->read(command.user, &value, 1u, &count, 50u) ==
             H2_PAL_ERR_CLOSED &&
         count == 0u);
  assert(command.vtable->write(command.user, &value, 1u, &count, 50u) ==
             H2_PAL_ERR_CLOSED &&
         count == 0u);
  assert(command.vtable->flush(command.user) == H2_PAL_ERR_CLOSED);
  assert(h2_device_serial_activate_pending(&transport) == H2_PAL_OK);
  assert(transport.conv == 22u && !transport.replacement_pending &&
         !transport.pending_conv);
  assert(s_last_ack == 22u);
  control(H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_CLOSE, 22u);
  assert(h2_device_serial_poll_session(&transport, 50u) == H2_PAL_OK);
  assert(transport.close_pending && transport.replacement_pending);
  assert(command.vtable->read(command.user, &value, 1u, &count, 50u) ==
         H2_PAL_ERR_CLOSED);
  h2_device_serial_deactivate(&transport);
  open_session(&transport, 33u);
  s_read_error = H2_PAL_ERR_CLOSED;
  assert(h2_device_serial_poll_session(&transport, 50u) == H2_PAL_ERR_CLOSED);
  assert(!transport.stream && !transport.pending_conv &&
         !transport.close_pending);
  s_read_error = 0;
  open_session(&transport, 44u);
  h2_device_serial_deinit(&transport);
  assert(!transport.stream);
  assert(transport.physical_io.read == physical_read);
}

static void test_flush_and_backpressure(void) {
  h2_device_serial_t transport;
  assert(h2_device_serial_init(&transport, &s_mem, &s_time, &s_physical) ==
         H2_PAL_OK);
  open_session(&transport, 55u);
  const h2_command_io_api_t command = h2_device_serial_io(&transport);
  uint8_t bytes[1024] = {0};
  size_t count;
  assert(command.vtable->write(command.user, bytes, 100u, &count, 30u) ==
             H2_PAL_OK &&
         count == 100u);
  uint64_t started = s_now;
  assert(command.vtable->flush(command.user) == H2_PAL_ERR_TIMEOUT);
  assert(s_now - started >= 60u && s_now - started < 200u);
  h2_device_serial_deactivate(&transport);
  open_session(&transport, 66u);
  h2_iostreamikcp_stats_t stats = {0};
  for (unsigned i = 0u; i < 80u && stats.waitsnd < 64u; ++i) {
    assert(command.vtable->write(command.user, bytes, sizeof(bytes), &count,
                                 30u) == H2_PAL_OK);
    assert(count == sizeof(bytes));
    assert(h2_iostreamikcp_get_stats(transport.stream, &stats) == H2_PAL_OK);
  }
  assert(stats.waitsnd >= 64u);
  started = s_now;
  assert(command.vtable->write(command.user, bytes, sizeof(bytes), &count,
                               30u) == H2_PAL_ERR_TIMEOUT);
  assert(count == 0u && s_now - started >= 30u && s_now - started < 100u);
  assert(h2_iostreamikcp_get_stats(transport.stream, &stats) == H2_PAL_OK &&
         stats.waitsnd >= 64u);
  control(H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN, 77u);
  assert(command.vtable->write(command.user, bytes, sizeof(bytes), &count,
                               30u) == H2_PAL_ERR_CLOSED);
  assert(count == 0u && transport.pending_conv == 77u);
  h2_device_serial_deinit(&transport);
}

int main(void) {
  test_endpoint();
  test_sessions();
  test_flush_and_backpressure();
  return 0;
}
