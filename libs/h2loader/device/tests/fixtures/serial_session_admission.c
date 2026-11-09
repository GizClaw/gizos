#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "h2_iostreamikcp.h"
#include "os/h2_pal_time.h"

#define H2_PHYSICAL_POLL_MS 10u
#define H2_WRITE_TIMEOUT_MS 5000u
#define H2_LOADER_APP_CLIENT_SESSION_RESET (-2)
#define H2_LOADER_APP_CLIENT_SESSION_CLOSED (-3)
#ifndef TEST_STREAM_RX_BUFFER
#define TEST_STREAM_RX_BUFFER 4096u
#define TEST_RECEIVE_WINDOW 20u
#endif

/* Only the physical boundary, clock and KCP allocation are faked. */
typedef struct {
  h2_iostreamikcp_io_t physical_io;
  h2_iostreamikcp_t* stream;
  const h2_pal_mem_api_t* allocator;
  const h2_pal_time_api_t* time;
  h2_iostreamikcp_filter_t filter;
  struct {
    size_t rx_buffer_size;
  } config;
  uint32_t conv, pending_conv, receive_window, last_frame_ms;
  int replacement_pending, close_pending, io_deadline_active;
} h2_device_serial_t;
typedef struct {
  h2_pal_result_t (*read)(void*, void*, size_t, size_t*, uint32_t);
} read_vtable_t;
typedef struct {
  h2_device_serial_t transport;
  struct {
    void* user;
    const read_vtable_t* vtable;
  } io;
} h2_device_app_serial_t;
static unsigned opens, closes, acks, delays, polls, live;
static uint32_t peer_open;
static h2_pal_result_t open_result = H2_PAL_OK, ack_result = H2_PAL_OK;
static char handle;
static uint32_t h2_device_serial_now(h2_device_serial_t* self) {
  (void)self;
  return 0u;
}
static uint32_t now_ms32(void* user) { return h2_device_serial_now(user); }
static int transport_stopped(const h2_device_serial_t* self) {
  (void)self;
  return 0;
}
static void deadline_start(h2_device_serial_t* self, uint32_t ms) {
  assert(ms == H2_WRITE_TIMEOUT_MS);
  self->io_deadline_active = 1;
}
static h2_pal_result_t transport_read(void* user, void* data, size_t len,
                                      size_t* out, uint32_t ms) {
  (void)user;
  (void)data;
  (void)len;
  (void)out;
  (void)ms;
  return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t transport_write(void* user, const void* data, size_t len,
                                       size_t* out, uint32_t ms) {
  (void)user;
  (void)data;
  (void)len;
  (void)out;
  (void)ms;
  return H2_PAL_ERR_UNSUPPORTED;
}
static h2_pal_result_t transport_flush(void* user) {
  (void)user;
  return H2_PAL_OK;
}
static h2_pal_result_t sleep_ms(void* user, uint32_t ms) {
  (void)user;
  assert(ms == H2_PHYSICAL_POLL_MS);
  ++delays;
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_vtable = {.sleep_ms = sleep_ms};
static const h2_pal_time_api_t time_api = {.vtable = &time_vtable};

h2_pal_result_t h2_iostreamikcp_open(const h2_iostreamikcp_config_t* config,
                                     h2_iostreamikcp_t** out) {
  assert(live == 0u && config->conv != 0u);
  assert(config->mtu == H2_IOSTREAMIKCP_DEFAULT_MTU);
  assert(config->rx_buffer_size == TEST_STREAM_RX_BUFFER &&
         config->receive_window == TEST_RECEIVE_WINDOW);
  ++opens;
  *out = NULL;
  if (open_result == H2_PAL_OK) {
    *out = (h2_iostreamikcp_t*)&handle;
    ++live;
  }
  return open_result;
}
void h2_iostreamikcp_close(h2_iostreamikcp_t* stream) {
  if (stream != NULL) {
    assert(stream == (h2_iostreamikcp_t*)&handle && live == 1u);
    --live;
    ++closes;
  }
}
void h2_iostreamikcp_filter_init(h2_iostreamikcp_filter_t* filter) {
  memset(filter, 0, sizeof(*filter));
}
static h2_pal_result_t send_control(h2_device_serial_t* transport,
                                    uint8_t flags, uint32_t conv) {
  assert(flags == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK);
  assert(transport->conv == conv && live == 1u);
  ++acks;
  return ack_result;
}
static h2_pal_result_t h2_device_serial_poll_session(
    h2_device_serial_t* transport, uint32_t timeout_ms) {
  assert(timeout_ms == 50u);
  ++polls;
  transport->pending_conv = peer_open;
  peer_open = 0u;
  return transport->pending_conv ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}
static h2_pal_result_t read_byte(void* user, void* out, size_t len,
                                 size_t* count, uint32_t timeout_ms) {
  (void)user;
  assert(len == 1u && timeout_ms == 50u);
  *(uint8_t*)out = 's';
  *count = 1u;
  return H2_PAL_OK;
}
/* FUNCTIONS */

static void assert_empty(const h2_device_serial_t* transport) {
  assert(transport->stream == NULL && transport->conv == 0u);
  assert(transport->pending_conv == 0u);
  assert(!transport->replacement_pending && !transport->close_pending);
  assert(live == 0u);
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const read_vtable_t io = {.read = read_byte};
  h2_device_app_serial_t state = {
      .transport = {.pending_conv = 7u,
                    .receive_window = TEST_RECEIVE_WINDOW,
                    .time = &time_api},
      .io = {.vtable = &io},
  };
#ifdef TEST_COMPACT
  state.transport.config.rx_buffer_size = TEST_STREAM_RX_BUFFER;
#endif
  if (strcmp(argv[1], "invalid") == 0) {
    assert(h2_device_serial_activate_pending(NULL) == H2_PAL_ERR_INVALID_STATE);
    state.transport.pending_conv = 0u;
    assert(h2_device_serial_activate_pending(&state.transport) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(opens == 0u && delays == 0u);
    return 0;
  }
  if (strcmp(argv[1], "success") != 0) {
    state.transport.stream = (h2_iostreamikcp_t*)&handle;
    state.transport.conv = 3u;
    state.transport.replacement_pending = state.transport.close_pending = 1;
    live = 1u;
    if (strcmp(argv[1], "ack") == 0)
      ack_result = H2_PAL_ERR_IO;
    else
      open_result = H2_PAL_ERR_NO_MEMORY;
    assert(app_read_byte(&state, 50u) == EOF);
    assert_empty(&state.transport);
    assert(opens == 1u && delays == 1u);
    assert(closes == (ack_result == H2_PAL_OK ? 1u : 2u));
    assert(acks == (ack_result == H2_PAL_OK ? 0u : 1u));
    for (unsigned i = 0u; i < 100u; ++i)
      assert(app_read_byte(&state, 50u) == EOF);
    assert(opens == 1u && polls == 100u && delays == 1u);
    if (strcmp(argv[1], "flood") == 0) {
      for (unsigned i = 0u; i < 100u; ++i) {
        peer_open = 7u;
        assert(app_read_byte(&state, 50u) == EOF);
        assert_empty(&state.transport);
      }
      assert(opens == 101u && delays == 101u && polls == 200u);
    }
    open_result = ack_result = H2_PAL_OK;
    peer_open = 7u;
  }
  unsigned previous_delays = delays;
  assert(app_read_byte(&state, 50u) == H2_LOADER_APP_CLIENT_SESSION_RESET);
  assert(state.transport.conv == 7u && state.transport.pending_conv == 0u);
  assert(app_read_byte(&state, 50u) == 's');
  state.transport.pending_conv = 9u;
  state.transport.replacement_pending = 1;
  assert(app_read_byte(&state, 50u) == H2_LOADER_APP_CLIENT_SESSION_RESET);
  assert(state.transport.conv == 9u && live == 1u);
  assert(delays == previous_delays);
  state.transport.close_pending = 1;
  assert(app_read_byte(&state, 50u) == H2_LOADER_APP_CLIENT_SESSION_RESET);
  assert_empty(&state.transport);
  return 0;
}
