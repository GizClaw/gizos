#include "h2_web_main_thread.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/threading.h>
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* clang-format off */
EM_JS(void, h2_web_test_set_serial_mode,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["i32"], null,
    (mode) => {
  const modes = [
    'normal', 'denied', 'normal', 'delayed', 'timeout-read', 'unplug-read',
    'grow-read', 'timeout-write', 'partial-read', 'unplug-write',
    'revoked-open', 'busy-open', 'close-needs-cancel-turn', 'delayed-read'
  ];
  globalThis.h2FakeSerialMode = modes[mode] || 'normal';
  globalThis.isSecureContext = mode !== 2;
  if (mode === 0 && globalThis.h2FakeSerialPort) {
    globalThis.h2FakeSerialPort.connected = true;
  }
});
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_test_forget_count,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32",
    () => { return globalThis.h2FakeForgetCount || 0; });
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_test_forget_release_ready,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32",
    () => typeof globalThis.h2ReleaseForget === 'function' ? 1 : 0);
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_test_close_rejected_before_cancel_settled,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32",
    () => { return globalThis.h2FakeCloseRejectedBeforeCancelSettled ? 1 : 0; });
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_test_audio_stopped_sources,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32",
    () => { return globalThis.h2FakeAudioStoppedSources || 0; });
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_test_audio_active_sources,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, [], "i32",
    () => { return globalThis.h2FakeAudioActiveSources || 0; });
});
/* clang-format on */

static void h2_web_test_task(void *user) {
  int *ran = user;
  *ran = 1;
}

typedef struct h2_web_auto_pump_test {
  h2_web_platform_t *platform;
  int complete;
} h2_web_auto_pump_test_t;

typedef struct h2_web_task_cancel_test {
  h2_web_platform_t *platform;
  h2_pal_result_t result;
} h2_web_task_cancel_test_t;

static void h2_web_test_task_cancel(void *user) {
  h2_web_task_cancel_test_t *test = user;
  test->result =
      h2_pal_time_sleep_ms(h2_web_platform_time_api(test->platform), 10000u);
}

static void h2_web_test_auto_pump(void *user) {
  h2_web_auto_pump_test_t *test = user;
  if (h2_pal_time_sleep_ms(h2_web_platform_time_api(test->platform), 1u) ==
      H2_PAL_OK) {
    test->complete = 1;
  }
}

static h2_web_auto_pump_test_t h2_web_auto_pump;
static h2_pal_task_t *h2_web_auto_pump_task;

static void h2_web_test_main_loop(void) {}

static int h2_web_test_http_header(void *user,
                                   const h2_pal_http_request_t *request,
                                   h2_pal_http_str_t name,
                                   h2_pal_http_str_t value) {
  (void)request;
  int *header_count = user;
  if (name.len == 0u || name.data == NULL ||
      (value.len != 0u && value.data == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *header_count += 1;
  return H2_PAL_OK;
}

typedef struct h2_web_webrtc_test {
  const h2_pal_webrtc_api_t *api;
  int local_sdp;
  int connected;
  int channel_open;
  int message;
} h2_web_webrtc_test_t;

static void h2_web_test_webrtc_peer_state(void *user,
                                          h2_pal_webrtc_peer_t *peer,
                                          h2_pal_webrtc_peer_state_t state) {
  (void)peer;
  h2_web_webrtc_test_t *test = user;
  if (state == H2_PAL_WEBRTC_PEER_CONNECTED)
    test->connected++;
}

static void h2_web_test_webrtc_local_sdp(void *user, h2_pal_webrtc_peer_t *peer,
                                         h2_pal_webrtc_sdp_type_t type,
                                         h2_pal_webrtc_str_t sdp) {
  (void)peer;
  h2_web_webrtc_test_t *test = user;
  if (type == H2_PAL_WEBRTC_SDP_OFFER && sdp.len == 10u &&
      memcmp(sdp.data, "fake-offer", 10u) == 0) {
    test->local_sdp++;
  }
}

static void
h2_web_test_webrtc_channel_state(void *user, h2_pal_webrtc_peer_t *peer,
                                 h2_pal_webrtc_channel_t *channel,
                                 const h2_pal_webrtc_channel_info_t *info,
                                 h2_pal_webrtc_channel_state_t state) {
  (void)peer;
  (void)channel;
  h2_web_webrtc_test_t *test = user;
  if (state == H2_PAL_WEBRTC_CHANNEL_OPEN && info->has_stream_id &&
      info->stream_id == 0u && info->ordered && info->reliable) {
    test->channel_open++;
  }
}

static void h2_web_test_webrtc_message(void *user, h2_pal_webrtc_peer_t *peer,
                                       h2_pal_webrtc_channel_t *channel,
                                       const h2_pal_webrtc_channel_info_t *info,
                                       const uint8_t *data, size_t len,
                                       int is_text) {
  (void)peer;
  (void)info;
  h2_web_webrtc_test_t *test = user;
  if (is_text && len == 4u && memcmp(data, "ping", 4u) == 0) {
    test->message++;
    h2_pal_webrtc_channel_close(test->api, channel);
  }
}

static void h2_web_test_webrtc_drain(h2_web_webrtc_test_t *test,
                                     h2_pal_webrtc_peer_t *peer) {
  h2_pal_webrtc_event_t event = {0};
  while (h2_pal_webrtc_peer_poll(test->api, peer, 0, &event) == H2_PAL_OK) {
    switch (event.kind) {
    case H2_PAL_WEBRTC_EVENT_PEER_STATE:
      h2_web_test_webrtc_peer_state(test, event.peer, event.peer_state);
      break;
    case H2_PAL_WEBRTC_EVENT_LOCAL_SDP:
      h2_web_test_webrtc_local_sdp(test, event.peer, event.sdp_type, event.sdp);
      break;
    case H2_PAL_WEBRTC_EVENT_CHANNEL_STATE:
      h2_web_test_webrtc_channel_state(test, event.peer, event.channel,
                                       &event.channel_info,
                                       event.channel_state);
      break;
    case H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE:
      h2_web_test_webrtc_message(test, event.peer, event.channel,
                                 &event.channel_info, event.data,
                                 event.data_len, event.is_text);
      break;
    default:
      break;
    }
    h2_pal_webrtc_event_release(&event);
  }
}

static void h2_web_test_auto_pump_verify(void *user) {
  (void)user;
  const h2_pal_result_t join_result =
      h2_pal_task_join(h2_web_platform_task_api(h2_web_auto_pump.platform),
      h2_web_auto_pump_task);
  const int success =
      h2_web_auto_pump.complete == 1 && join_result == H2_PAL_OK;
  h2_web_platform_destroy(h2_web_auto_pump.platform);
  h2_web_auto_pump.platform = NULL;
  if (!success) {
    fprintf(stderr, "Web PAL auto-pump verification failed\n");
    emscripten_force_exit(38);
  }
  emscripten_cancel_main_loop();
  puts("Web PAL tests passed");
  emscripten_force_exit(0);
}

typedef struct h2_web_serial_test {
  h2_web_platform_t *platform;
  int result;
  _Atomic int shutdown_checked;
} h2_web_serial_test_t;

/* Authorization IDs are invalidated by forget/re-authorize, not reused. */
static char h2_web_test_serial_port[H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN];

static void h2_web_test_serial(void *user) {
  h2_web_serial_test_t *test = user;
  const h2_pal_serial_host_api_t *serial =
      h2_web_platform_serial_host_api(test->platform);
  h2_pal_serial_host_snapshot_t *snapshot = NULL;
  size_t count = 0u;
  h2_pal_serial_host_port_info_t info;
  h2_pal_serial_host_session_t *session = NULL;
  const h2_pal_uart_io_stream_api_t *stream = NULL;
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = 230400u,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = H2_PAL_UART_PARITY_NONE,
      .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
      .rx_buffer_size = 4096u,
      .tx_buffer_size = 4096u,
  };
  unsigned char bytes[2] = {0u, 0u};
  size_t transferred = 0u;
  test->result = 1;
  h2_pal_uart_io_stream_config_t invalid_config = config;
  invalid_config.stop_bits = 3u;
  if (serial->vtable->open(serial->user, "web-serial-1", &invalid_config,
                           &session) != H2_PAL_ERR_INVALID_ARG ||
      session != NULL) {
    return;
  }
  invalid_config = config;
  invalid_config.parity = (h2_pal_uart_parity_t)99;
  if (serial->vtable->open(serial->user, "web-serial-1", &invalid_config,
                           &session) != H2_PAL_ERR_INVALID_ARG ||
      session != NULL) {
    return;
  }
  if (h2_pal_serial_host_open(serial, "web-serial-999999999999999999999999",
                              &config, &session) != H2_PAL_ERR_INVALID_ARG ||
      session != NULL) {
    return;
  }
  if (h2_pal_serial_host_scan(serial, &snapshot) != H2_PAL_OK ||
      h2_pal_serial_host_snapshot_count(serial, snapshot, &count) !=
          H2_PAL_OK ||
      count != 1u ||
      h2_pal_serial_host_snapshot_get(serial, snapshot, 0u, &info) !=
          H2_PAL_OK ||
      strcmp(info.port_id, h2_web_test_serial_port) != 0 ||
      h2_pal_serial_host_snapshot_destroy(serial, &snapshot) != H2_PAL_OK) {
    return;
  }
  test->result = 2;
  if (h2_pal_serial_host_open(serial, info.port_id, &config, &session) !=
          H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK ||
      h2_pal_uart_io_stream_read(stream, bytes, sizeof(bytes), &transferred,
                                 1000u) != H2_PAL_OK ||
      transferred != sizeof(bytes) || bytes[0] != 0x48u || bytes[1] != 0x32u) {
    (void)h2_pal_serial_host_close(serial, &session);
    return;
  }
  test->result = 3;
  h2_pal_uart_io_stream_config_t unsupported_config = config;
  unsupported_config.data_bits = 6u;
  h2_pal_uart_io_stream_config_t invalid_configure = config;
  invalid_configure.parity = (h2_pal_uart_parity_t)99;
  uint32_t control_lines = UINT32_MAX;
  if (h2_pal_uart_io_stream_write(stream, bytes, sizeof(bytes), &transferred,
                                  1000u) != H2_PAL_OK ||
      transferred != sizeof(bytes) ||
      h2_pal_uart_io_stream_flush(stream) != H2_PAL_OK ||
      h2_pal_uart_io_stream_configure(stream, &unsupported_config) !=
          H2_PAL_ERR_UNSUPPORTED ||
      stream->vtable->configure(stream->user, &invalid_configure) !=
          H2_PAL_ERR_INVALID_ARG ||
      h2_pal_serial_host_set_control_lines(
          serial, session, H2_PAL_SERIAL_HOST_CONTROL_DTR,
          H2_PAL_SERIAL_HOST_CONTROL_DTR) != H2_PAL_OK ||
      h2_pal_serial_host_get_control_lines(serial, session, &control_lines) !=
          H2_PAL_ERR_UNSUPPORTED ||
      control_lines != 0u ||
      h2_pal_serial_host_close(serial, &session) != H2_PAL_OK) {
    return;
  }
  test->result = 0;
}

typedef struct h2_web_serial_read_test {
  h2_web_platform_t *platform;
  int mode;
  h2_pal_result_t expected;
  int result;
} h2_web_serial_read_test_t;

static void h2_web_test_serial_read_edge(void *user) {
  h2_web_serial_read_test_t *test = user;
  const h2_pal_serial_host_api_t *serial =
      h2_web_platform_serial_host_api(test->platform);
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = 230400u,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = H2_PAL_UART_PARITY_NONE,
      .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
      .rx_buffer_size = 4096u,
      .tx_buffer_size = 4096u,
  };
  h2_pal_serial_host_session_t *session = NULL;
  const h2_pal_uart_io_stream_api_t *stream = NULL;
  unsigned char bytes[2] = {0u, 0u};
  size_t transferred = 99u;
  test->result = 1;
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  if (h2_pal_serial_host_open(serial, h2_web_test_serial_port, &config,
                              &session) != H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK) {
    return;
  }
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){test->mode}});
  const h2_pal_result_t result = h2_pal_uart_io_stream_read(
      stream, bytes, sizeof(bytes), &transferred, 1u);
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  const h2_pal_result_t close_result =
      h2_pal_serial_host_close(serial, &session);
  if (result != test->expected || close_result != H2_PAL_OK)
    return;
  if (result == H2_PAL_OK && (transferred != sizeof(bytes) ||
                              bytes[0] != 0x48u || bytes[1] != 0x32u)) {
    return;
  }
  if (result != H2_PAL_OK && transferred != 0u)
    return;
  test->result = 0;
}

static void h2_web_test_serial_write_timeout(void *user) {
  h2_web_serial_read_test_t *test = user;
  const h2_pal_serial_host_api_t *serial =
      h2_web_platform_serial_host_api(test->platform);
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = 230400u,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = H2_PAL_UART_PARITY_NONE,
      .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
      .rx_buffer_size = 4096u,
      .tx_buffer_size = 4096u,
  };
  h2_pal_serial_host_session_t *session = NULL;
  const h2_pal_uart_io_stream_api_t *stream = NULL;
  const unsigned char bytes[2] = {0x48u, 0x32u};
  size_t transferred = 99u;
  test->result = 1;
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  if (h2_pal_serial_host_open(serial, h2_web_test_serial_port, &config,
                              &session) != H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK) {
    return;
  }
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){7}});
  const h2_pal_result_t timeout_result = h2_pal_uart_io_stream_write(
      stream, bytes, sizeof(bytes), &transferred, 1u);
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  const h2_pal_result_t terminal_result = h2_pal_uart_io_stream_write(
      stream, bytes, sizeof(bytes), &transferred, 100u);
  const h2_pal_result_t close_result =
      h2_pal_serial_host_close(serial, &session);
  if (timeout_result != H2_PAL_ERR_TIMEOUT || transferred != 0u ||
      terminal_result != H2_PAL_ERR_CLOSED || close_result != H2_PAL_OK) {
    return;
  }
  test->result = 0;
}

static void h2_web_test_serial_read_timeout_recovery(void *user) {
  h2_web_serial_read_test_t *test = user;
  const h2_pal_serial_host_api_t *serial =
      h2_web_platform_serial_host_api(test->platform);
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = 230400u,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = H2_PAL_UART_PARITY_NONE,
      .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
      .rx_buffer_size = 4096u,
      .tx_buffer_size = 4096u,
  };
  h2_pal_serial_host_session_t *session = NULL;
  const h2_pal_uart_io_stream_api_t *stream = NULL;
  unsigned char bytes[2] = {0u, 0u};
  size_t transferred = 99u;
  test->result = 1;
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  if (h2_pal_serial_host_open(serial, h2_web_test_serial_port, &config,
                              &session) != H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK) {
    return;
  }
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){13}});
  const h2_pal_result_t timeout_result = h2_pal_uart_io_stream_read(
      stream, bytes, sizeof(bytes), &transferred, 1u);
  const h2_pal_result_t recovery_result = h2_pal_uart_io_stream_read(
      stream, bytes, sizeof(bytes), &transferred, 100u);
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  const h2_pal_result_t close_result =
      h2_pal_serial_host_close(serial, &session);
  if (timeout_result != H2_PAL_ERR_TIMEOUT || recovery_result != H2_PAL_OK ||
      transferred != sizeof(bytes) || bytes[0] != 0x48u || bytes[1] != 0x32u ||
      close_result != H2_PAL_OK) {
    return;
  }
  test->result = 0;
}

static void h2_web_test_serial_partial_read(void *user) {
  h2_web_serial_read_test_t *test = user;
  const h2_pal_serial_host_api_t *serial =
      h2_web_platform_serial_host_api(test->platform);
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = 230400u,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = H2_PAL_UART_PARITY_NONE,
      .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
      .rx_buffer_size = 4096u,
      .tx_buffer_size = 4096u,
  };
  h2_pal_serial_host_session_t *session = NULL;
  const h2_pal_uart_io_stream_api_t *stream = NULL;
  unsigned char first[2] = {0u, 0u};
  unsigned char second[2] = {0u, 0u};
  size_t transferred = 0u;
  test->result = 1;
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  if (h2_pal_serial_host_open(serial, h2_web_test_serial_port, &config,
                              &session) != H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK) {
    return;
  }
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){8}});
  const h2_pal_result_t first_result = h2_pal_uart_io_stream_read(
      stream, first, sizeof(first), &transferred, 100u);
  const h2_pal_result_t second_result = h2_pal_uart_io_stream_read(
      stream, second, sizeof(second), &transferred, 100u);
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  const h2_pal_result_t close_result =
      h2_pal_serial_host_close(serial, &session);
  if (first_result != H2_PAL_OK || second_result != H2_PAL_OK ||
      transferred != 2u || first[0] != 0x48u || first[1] != 0x32u ||
      second[0] != 0x21u || second[1] != 0x22u || close_result != H2_PAL_OK) {
    return;
  }
  test->result = 0;
}

static void h2_web_test_serial_write_unplug(void *user) {
  h2_web_serial_read_test_t *test = user;
  const h2_pal_serial_host_api_t *serial =
      h2_web_platform_serial_host_api(test->platform);
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = 230400u,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = H2_PAL_UART_PARITY_NONE,
      .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
      .rx_buffer_size = 4096u,
      .tx_buffer_size = 4096u,
  };
  h2_pal_serial_host_session_t *session = NULL;
  const h2_pal_uart_io_stream_api_t *stream = NULL;
  const unsigned char bytes[2] = {0x48u, 0x32u};
  size_t transferred = 99u;
  test->result = 1;
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  if (h2_pal_serial_host_open(serial, h2_web_test_serial_port, &config,
                              &session) != H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK) {
    return;
  }
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){9}});
  const h2_pal_result_t write_result = h2_pal_uart_io_stream_write(
      stream, bytes, sizeof(bytes), &transferred, 100u);
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  const h2_pal_result_t close_result =
      h2_pal_serial_host_close(serial, &session);
  if (write_result != H2_PAL_ERR_CLOSED || transferred != 0u ||
      close_result != H2_PAL_OK) {
    return;
  }
  test->result = 0;
}

static void h2_web_test_serial_open_failure(void *user) {
  h2_web_serial_read_test_t *test = user;
  const h2_pal_serial_host_api_t *serial =
      h2_web_platform_serial_host_api(test->platform);
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = 230400u,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = H2_PAL_UART_PARITY_NONE,
      .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
      .rx_buffer_size = 4096u,
      .tx_buffer_size = 4096u,
  };
  h2_pal_serial_host_session_t *session = NULL;
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){test->mode}});
  const h2_pal_result_t result = h2_pal_serial_host_open(
      serial, h2_web_test_serial_port, &config, &session);
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  test->result = result == test->expected && session == NULL ? 0 : 1;
}

typedef struct h2_web_serial_close_wait_state {
  const h2_pal_uart_io_stream_api_t *stream;
  h2_pal_result_t read_result;
} h2_web_serial_close_wait_state_t;

static void h2_web_test_serial_blocked_read(void *user) {
  h2_web_serial_close_wait_state_t *state = user;
  unsigned char byte = 0u;
  size_t transferred = 0u;
  state->read_result = h2_pal_uart_io_stream_read(
      state->stream, &byte, sizeof(byte), &transferred, 5u);
}

static void h2_web_test_serial_shutdown(void *user) {
  h2_web_serial_test_t *test = user;
  const h2_pal_serial_host_api_t *serial =
      h2_web_platform_serial_host_api(test->platform);
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = 230400u,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = H2_PAL_UART_PARITY_NONE,
      .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
      .rx_buffer_size = 4096u,
      .tx_buffer_size = 4096u,
  };
  h2_pal_serial_host_session_t *session = NULL;
  const h2_pal_uart_io_stream_api_t *stream = NULL;
  unsigned char byte = 0u;
  size_t transferred = 0u;
  test->result = 1;
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  if (h2_pal_serial_host_open(serial, h2_web_test_serial_port, &config,
                              &session) != H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK) {
    return;
  }
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){4}});
  const h2_pal_result_t read_result = h2_pal_uart_io_stream_read(
      stream, &byte, sizeof(byte), &transferred, 10000u);
  // shutdown wakes this reader before its main-thread force-close finishes.
  // Keep the fake failure mode and session alive until the driver has checked
  // that operation; cleanup must not race with the assertion under coverage.
  while (!atomic_load(&test->shutdown_checked))
    h2_web_worker_sleep(1u);
  const h2_pal_result_t close_result =
      h2_pal_serial_host_close(serial, &session);
  if (read_result != H2_PAL_ERR_CLOSED || transferred != 0u ||
      close_result != H2_PAL_OK || session != NULL) {
    return;
  }
  test->result = 0;
}

static void h2_web_test_serial_close_wait(void *user) {
  h2_web_serial_read_test_t *test = user;
  const h2_pal_serial_host_api_t *serial =
      h2_web_platform_serial_host_api(test->platform);
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = 230400u,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = H2_PAL_UART_PARITY_NONE,
      .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
      .rx_buffer_size = 4096u,
      .tx_buffer_size = 4096u,
  };
  h2_pal_serial_host_session_t *session = NULL;
  const h2_pal_uart_io_stream_api_t *stream = NULL;
  h2_pal_task_t *reader = NULL;
  h2_web_serial_close_wait_state_t state = {
      .read_result = H2_PAL_ERR_INVALID_STATE,
  };
  test->result = 1;
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  if (h2_pal_serial_host_open(serial, h2_web_test_serial_port, &config,
                              &session) != H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK) {
    return;
  }
  state.stream = stream;
  /* clang-format off */
MAIN_THREAD_EM_ASM({ globalThis.h2FakePendingReadResolve = null; });
/* clang-format on */
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){4}});
  if (h2_pal_task_start(h2_web_platform_task_api(test->platform), NULL,
                        h2_web_test_serial_blocked_read, &state,
                        &reader) != H2_PAL_OK) {
    return;
  }
  const double read_deadline = emscripten_get_now() + 3000.0;
  while (!/* clang-format off */
MAIN_THREAD_EM_ASM_INT({ return !!globalThis.h2FakePendingReadResolve; })
/* clang-format on */ && emscripten_get_now() < read_deadline)
    h2_web_worker_sleep(1u);
  if (!/* clang-format off */
MAIN_THREAD_EM_ASM_INT({ return !!globalThis.h2FakePendingReadResolve; })
/* clang-format on */)
    return;
  const h2_pal_result_t close_result =
      h2_pal_serial_host_close(serial, &session);
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  const h2_pal_result_t join_result =
      h2_pal_task_join(h2_web_platform_task_api(test->platform), reader);
  if (close_result != H2_PAL_OK || join_result != H2_PAL_OK ||
      state.read_result != H2_PAL_ERR_TIMEOUT || session != NULL) {
    return;
  }
  test->result = 0;
}

typedef struct h2_web_timer_mutation_test {
  const h2_pal_timer_api_t *api;
  h2_pal_timer_t *victim;
  _Atomic int callbacks;
  _Atomic int result;
  _Atomic int done;
} h2_web_timer_mutation_test_t;

static void h2_web_test_timer_victim(void *user, h2_pal_timer_t *timer) {
  (void)timer;
  h2_web_timer_mutation_test_t *test = user;
  test->result = 1;
}

static void h2_web_test_timer_count(void *user, h2_pal_timer_t *timer) {
  (void)timer;
  ++*(_Atomic int *)user;
}

static void h2_web_test_timer_destroyer(void *user, h2_pal_timer_t *timer) {
  h2_web_timer_mutation_test_t *test = user;
  ++test->callbacks;
  if (h2_pal_timer_destroy(test->api, test->victim) != H2_PAL_OK) {
    test->result = 2;
    return;
  }
  test->victim = NULL;
  if (h2_pal_timer_destroy(test->api, timer) != H2_PAL_OK) {
    test->result = 3;
  }
  test->done = 1;
}

static int h2_web_test_run_task(h2_web_platform_t *platform,
                                h2_pal_task_entry_t entry, void *user) {
  h2_pal_task_t *task = NULL;
  if (h2_pal_task_start(h2_web_platform_task_api(platform), NULL, entry, user,
                        &task) != H2_PAL_OK) {
    return 0;
  }
  for (int iteration = 0; iteration < 64; ++iteration) {
    if (h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK)
      return 0;
    if (h2_pal_task_join(h2_web_platform_task_api(platform), task) ==
        H2_PAL_OK) {
      return 1;
    }
    h2_web_worker_sleep(1u);
  }
  return 0;
}

typedef struct h2_web_decoder_test {
  h2_web_platform_t *platform;
  int result;
} h2_web_decoder_test_t;

static void h2_web_test_decoders(void *user) {
  h2_web_decoder_test_t *test = user;
  const h2_pal_mem_api_t *allocator = h2_web_platform_mem_api();
  const h2_pal_video_decoder_api_t *video =
      h2_web_platform_video_decoder_api(test->platform);
  const h2_video_decoder_config_t video_open = {
      .frame_allocator = allocator,
      .preferred_format = H2_VIDEO_PIXEL_FORMAT_RGB565,
  };
  const uint8_t h264_config[] = {0u, 0u, 0u, 1u, 0x67u, 0x42u, 0xe0u, 0x1eu};
  const h2_video_decoder_stream_config_t video_stream = {
      .codec = H2_VIDEO_CODEC_H264,
      .bitstream_format = H2_VIDEO_BITSTREAM_H264_ANNEX_B,
      .coded_width = 2u,
      .coded_height = 2u,
      .visible_width = 2u,
      .visible_height = 2u,
      .codec_config = h264_config,
      .codec_config_size = sizeof(h264_config),
  };
  const uint8_t h264_packet[] = {0u, 0u, 0u, 1u, 0x65u, 0x88u};
  const h2_video_decoder_packet_t video_packet = {
      .data = h264_packet,
      .size = sizeof(h264_packet),
      .pts_us = 1000,
      .duration_us = 33000,
  };
  h2_pal_video_decoder_session_t *video_session = NULL;
  h2_pal_video_decoder_frame_t *video_frame = NULL;
  h2_video_frame_info_t video_info = {0};
  test->result = 1;
  if (h2_pal_video_decoder_open(video, &video_open, &video_session) != H2_PAL_OK ||
      h2_pal_video_decoder_configure(video, video_session, &video_stream) !=
          H2_PAL_OK ||
      h2_pal_video_decoder_submit_packet(video, video_session, &video_packet) !=
          H2_PAL_OK ||
      h2_pal_video_decoder_acquire_frame(video, video_session, 100u,
                                         &video_frame) != H2_PAL_OK ||
      h2_pal_video_decoder_frame_get_info(video, video_session, video_frame,
                                          &video_info) != H2_PAL_OK) {
    return;
  }
  const uint16_t *pixels = video_info.planes[0].data;
  if (video_info.format != H2_VIDEO_PIXEL_FORMAT_RGB565 ||
      video_info.width != 2u || video_info.height != 2u ||
      video_info.plane_count != 1u || video_info.planes[0].bytes != 8u ||
      pixels[0] != 0xf800u || pixels[1] != 0x07e0u ||
      pixels[2] != 0x001fu || pixels[3] != 0xffffu ||
      h2_pal_video_decoder_release_frame(video, video_session, video_frame) !=
          H2_PAL_OK) {
    return;
  }
  const h2_video_decoder_packet_t video_eos = {
      .flags = H2_VIDEO_DECODER_PACKET_END_OF_STREAM,
  };
  if (h2_pal_video_decoder_submit_packet(video, video_session, &video_eos) !=
          H2_PAL_OK ||
      h2_pal_video_decoder_acquire_frame(video, video_session, 100u,
                                         &video_frame) != H2_PAL_EXIT ||
      h2_pal_video_decoder_close(video, video_session) != H2_PAL_OK) {
    return;
  }

  const h2_pal_audio_decoder_api_t *audio =
      h2_web_platform_audio_decoder_api(test->platform);
  const h2_audio_decoder_config_t audio_open = {
      .pcm_allocator = allocator,
      .preferred_format = H2_AUDIO_SAMPLE_S16LE,
  };
  const uint8_t audio_specific_config[] = {0x14u, 0x08u};
  const h2_audio_decoder_stream_config_t audio_stream = {
      .codec = H2_AUDIO_CODEC_AAC_LC,
      .bitstream_format = H2_AUDIO_BITSTREAM_AAC_RAW,
      .sample_rate_hz = 16000u,
      .channels = 1u,
      .codec_config = audio_specific_config,
      .codec_config_size = sizeof(audio_specific_config),
  };
  const uint8_t aac_packet[] = {0x01u, 0x02u};
  const h2_audio_decoder_packet_t audio_packet = {
      .data = aac_packet,
      .size = sizeof(aac_packet),
      .pts_us = 2000,
      .duration_us = 125,
  };
  h2_pal_audio_decoder_session_t *audio_session = NULL;
  h2_pal_audio_decoder_frame_t *audio_frame = NULL;
  h2_audio_decoder_frame_info_t audio_info = {0};
  if (h2_pal_audio_decoder_open(audio, &audio_open, &audio_session) != H2_PAL_OK ||
      h2_pal_audio_decoder_configure(audio, audio_session, &audio_stream) !=
          H2_PAL_OK ||
      h2_pal_audio_decoder_submit_packet(audio, audio_session, &audio_packet) !=
          H2_PAL_OK ||
      h2_pal_audio_decoder_acquire_frame(audio, audio_session, 100u,
                                         &audio_frame) != H2_PAL_OK ||
      h2_pal_audio_decoder_frame_get_info(audio, audio_session, audio_frame,
                                          &audio_info) != H2_PAL_OK) {
    return;
  }
  const int16_t *samples = audio_info.data;
  if (audio_info.sample_format != H2_AUDIO_SAMPLE_S16LE ||
      audio_info.sample_rate_hz != 16000u || audio_info.channels != 1u ||
      audio_info.samples_per_channel != 2u || audio_info.bytes != 4u ||
      samples[0] != 1000 || samples[1] != -1000 ||
      h2_pal_audio_decoder_release_frame(audio, audio_session, audio_frame) !=
          H2_PAL_OK) {
    return;
  }
  const h2_audio_decoder_packet_t audio_eos = {
      .flags = H2_AUDIO_DECODER_PACKET_END_OF_STREAM,
  };
  if (h2_pal_audio_decoder_submit_packet(audio, audio_session, &audio_eos) !=
          H2_PAL_OK ||
      h2_pal_audio_decoder_acquire_frame(audio, audio_session, 100u,
                                         &audio_frame) != H2_PAL_EXIT ||
      h2_pal_audio_decoder_close(audio, audio_session) != H2_PAL_OK) {
    return;
  }
  test->result = 0;
}

static h2_pal_result_t wait_authorization(h2_web_platform_t *platform,
                                          char *port, size_t size) {
  h2_pal_result_t rc;
  const double deadline = emscripten_get_now() + 1000.0;
  do {
    rc = h2_web_platform_serial_authorization(platform, port, size);
    if (rc != H2_PAL_ERR_WOULD_BLOCK)
      return rc;
    h2_web_worker_sleep(1u);
  } while (emscripten_get_now() < deadline);
  return rc;
}

/* The generic entry preserves caller-owned types and supports direct/nested
 * UI dispatch as well as simultaneous Worker callers. */
typedef struct {
  uint64_t wide;
  double fraction;
  const char *text;
  unsigned calls;
  int result;
} main_call_test_context_t;

static void main_call_nested(void *context, h2_web_main_result_t *result,
                             h2_web_main_completion_t *completion) {
  main_call_test_context_t *args = context;
  if (!emscripten_is_main_runtime_thread())
    args->result = 1;
  ++args->calls;
  result->u64 = args->wide;
  h2_web_main_complete(completion);
}

static void main_call_dispatch(void *context, h2_web_main_result_t *result,
                               h2_web_main_completion_t *completion) {
  main_call_test_context_t *args = context;
  if (!emscripten_is_main_runtime_thread() ||
      args->wide != UINT64_C(0xfedcba9876543210) || args->fraction != 1.25 ||
      strcmp(args->text, "main-call") != 0)
    args->result = 1;
  if (h2_web_main_call(main_call_nested, args).u64 != args->wide)
    args->result = 1;
  args->fraction *= 2;
  ++args->calls;
  result->u64 = args->wide;
  h2_web_main_complete(completion);
}

static void *main_call_worker(void *context) {
  main_call_test_context_t *args = context;
  if (h2_web_main_call(main_call_dispatch, context).u64 != args->wide)
    args->result = 1;
  return NULL;
}
/* clang-format off */
EM_JS(void, main_call_values,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion,
    ["i32", "u16", "u32", "u64", "double", "pointer"], "double",
    (negative, small, unsigned, wide, fraction, text) => {
      if (negative !== -7 || small !== 65535 || unsigned !== 0xfedcba98 ||
          wide !== 0xfedcba9876543210n || UTF8ToString(text) !== "main-call")
        return NaN;
      return fraction * 4;
    });
});
/* clang-format on */
/* clang-format off */
EM_JS(void, main_call_pointer,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["pointer"], "pointer", pointer => pointer);
});
/* clang-format on */
/* clang-format off */
EM_JS(void, main_call_promise,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["i32"], "i32", async value => {
    await new Promise(resolve => setTimeout(resolve, 1));
    return value + 2;
  });
});
/* clang-format on */
static int test_main_call(void) {
  if (emscripten_is_main_runtime_thread())
    return 1;
  int negative = -7;
  uint16_t small = UINT16_MAX;
  uint32_t unsigned_value = UINT32_C(0xfedcba98);
  uint64_t wide = UINT64_C(0xfedcba9876543210);
  double fraction = 1.25;
  const char *text = "main-call";
  const void *values[] = {&negative, &small,    &unsigned_value,
                          &wide,     &fraction, &text};
  if (h2_web_main_call(main_call_values, values).f64 != 5.0 ||
      h2_web_main_call(main_call_pointer, (const void *[]){&text}).ptr !=
          text ||
      h2_web_main_call(main_call_promise, (const void *[]){&negative}).i32 !=
          -5)
    return 1;
  main_call_test_context_t contexts[2];
  pthread_t threads[2];
  for (unsigned i = 0; i < 2; ++i) {
    contexts[i] =
        (main_call_test_context_t){.wide = UINT64_C(0xfedcba9876543210),
                                   .fraction = 1.25,
                                   .text = "main-call"};
    if (pthread_create(&threads[i], NULL, main_call_worker, &contexts[i]) != 0)
      abort();
  }
  for (unsigned i = 0; i < 2; ++i) {
    if (pthread_join(threads[i], NULL) != 0)
      abort();
    if (contexts[i].calls != 2 || contexts[i].fraction != 2.5 ||
        contexts[i].result != 0)
      return 1;
  }
  return 0;
}

static int run_tests(void) {
  if (test_main_call() != 0)
    return 90;
  const h2_web_platform_config_t pixel_count_overflow = {
      .display_width = INT32_MAX,
      .display_height = INT32_MAX,
  };
  if (h2_web_platform_create(&pixel_count_overflow) != NULL) {
    return 1;
  }

  const h2_web_platform_config_t byte_count_overflow = {
      .display_width = INT32_MAX,
      .display_height = 2,
  };
  if (h2_web_platform_create(&byte_count_overflow) != NULL) {
    return 2;
  }

  const h2_web_platform_config_t valid = {
      .display_width = 2,
      .display_height = 2,
  };
  h2_web_platform_t *platform = h2_web_platform_create(&valid);
  if (platform == NULL) {
    return 3;
  }
  if (h2_web_platform_log_api() == NULL ||
      h2_web_platform_time_api(platform) == NULL ||
      h2_web_platform_timer_api(platform) == NULL ||
      h2_web_platform_task_api(platform) == NULL ||
      h2_web_platform_queue_api(platform) == NULL ||
      h2_web_platform_sync_api(platform) == NULL ||
      h2_web_platform_display_api(platform) == NULL ||
      h2_web_platform_touch_api(platform) == NULL ||
      h2_web_platform_pref_api(platform) == NULL ||
      h2_web_platform_http_api(platform) == NULL ||
      h2_web_platform_crypto_api(platform) == NULL ||
      h2_web_platform_audio_api(platform) == NULL ||
      h2_web_platform_audio_decoder_api(platform) == NULL ||
      h2_web_platform_video_decoder_api(platform) == NULL ||
      h2_web_platform_serial_host_api(platform) == NULL ||
      h2_web_platform_webrtc_api(platform) == NULL) {
    return 4;
  }
  h2_web_decoder_test_t decoder_test = {.platform = platform, .result = 1};
  if (!h2_web_test_run_task(platform, h2_web_test_decoders, &decoder_test) ||
      decoder_test.result != 0) {
    return 107;
  }
  const h2_pal_time_api_t *clock = h2_web_platform_time_api(platform);
  uint64_t wall_ms = 0u, monotonic_us = 0u;
  h2_pal_time_wall_status_t wall_status = {0};
  /* clang-format off */
MAIN_THREAD_EM_ASM({
    globalThis.h2SavedDateNow = Date.now;
    Date.now = () => 1700000000123;
  });
/* clang-format on */
  if (h2_pal_time_get_wall_ms(clock, &wall_ms) != H2_PAL_OK ||
      wall_ms != UINT64_C(1700000000123) ||
      h2_pal_time_get_wall_status(clock, &wall_status) != H2_PAL_OK ||
      !wall_status.valid ||
      wall_status.source != H2_PAL_TIME_WALL_SOURCE_UNKNOWN ||
      h2_pal_time_get_monotonic_us(clock, &monotonic_us) != H2_PAL_OK ||
      h2_pal_time_set_wall_ms(clock, 0u) != H2_PAL_OK ||
      h2_pal_time_get_wall_ms(clock, &wall_ms) != H2_PAL_OK || wall_ms != 0u ||
      h2_pal_time_get_wall_status(clock, &wall_status) != H2_PAL_OK ||
      wall_status.source != H2_PAL_TIME_WALL_SOURCE_USER ||
      h2_pal_time_set_wall_ms(clock, UINT64_C(1700000000123)) != H2_PAL_OK)
    return 51;
  /* clang-format off */
MAIN_THREAD_EM_ASM({ Date.now = () => NaN; });
/* clang-format on */
  if (h2_pal_time_get_wall_ms(clock, &wall_ms) != H2_PAL_ERR_UNAVAILABLE ||
      wall_ms != 0u ||
      h2_pal_time_get_wall_status(clock, &wall_status) != H2_PAL_ERR_UNAVAILABLE ||
      wall_status.valid)
    return 52;
  /* clang-format off */
MAIN_THREAD_EM_ASM({ Date.now = globalThis.h2SavedDateNow; });
/* clang-format on */
  uint8_t random_bytes[32] = {0};
  h2_pal_x25519_keypair_t keypair = {0};
  if (h2_pal_crypto_random(h2_web_platform_crypto_api(platform), random_bytes,
                           sizeof(random_bytes)) != H2_PAL_OK ||
      h2_pal_crypto_x25519_keypair_generate(
          h2_web_platform_crypto_api(platform), &keypair) != H2_PAL_OK) {
    return 103;
  }
  int http_header_count = 0;
  const h2_pal_http_request_t http_request = {
      .method = H2_PAL_HTTP_GET,
      .url = {.data = "data:text/plain,h2-web-http", .len = 27u},
      .response_header_cb = h2_web_test_http_header,
      .response_header_user = &http_header_count,
      .timeout_ms = 1000,
      .response_allocator = h2_web_platform_mem_api(),
  };
  h2_pal_http_response_t http_response;
  h2_pal_http_response_reset(&http_response);
  if (h2_pal_http_request(h2_web_platform_http_api(platform), &http_request,
                          &http_response) != H2_PAL_OK ||
      http_response.status_code != 200 || http_response.body_len != 11u ||
      memcmp(http_response.body, "h2-web-http", 11u) != 0 ||
      http_header_count == 0) {
    return 104;
  }
  h2_pal_http_response_free(h2_web_platform_http_api(platform),
                            &http_response);
  // Body failures happen after headers arrive. Repeated retries must not retain
  // Wasm header allocations, including when AbortController times out the body.
  /* clang-format off */
MAIN_THREAD_EM_ASM({
    globalThis.h2SavedFetch = globalThis.fetch;
    globalThis.fetch = async (_url, options) => ({
      status: 200,
      headers: new Headers({'x-large-header': 'x'.repeat(16384)}),
      arrayBuffer: () => globalThis.h2HttpBodyTimeout
        ? new Promise((_resolve, reject) => options.signal.addEventListener(
            'abort', () => reject(new DOMException('aborted', 'AbortError')),
            {once: true}))
        : Promise.reject(new Error('body transport failed')),
    });
  });
/* clang-format on */
  for (int timeout = 0; timeout < 2; ++timeout) {
    /* clang-format off */
MAIN_THREAD_EM_ASM({ globalThis.h2HttpBodyTimeout = !!$0; }, timeout);
/* clang-format on */
    h2_pal_http_request_t failed_request = http_request;
    failed_request.timeout_ms = timeout ? 50 : 2000;
    const int expected = timeout ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
    // Give an immediate transport error its own budget; a loaded Worker
    // must not turn the IO assertion into an unrelated 10 ms deadline race.
    if (h2_pal_http_request(h2_web_platform_http_api(platform), &failed_request,
                           &http_response) != expected)
      return 108;
    const size_t allocated = mallinfo().uordblks;
    for (int attempt = 0; attempt < 3; ++attempt) {
      if (h2_pal_http_request(h2_web_platform_http_api(platform), &failed_request,
                             &http_response) != expected ||
          http_response.body != NULL || http_response.status_code != 0)
        return 109;
    }
    if ((size_t)mallinfo().uordblks > allocated)
      return 110;
  }
  /* clang-format off */
MAIN_THREAD_EM_ASM({ globalThis.fetch = globalThis.h2SavedFetch; });
/* clang-format on */
  const h2_pal_audio_api_t *audio = h2_web_platform_audio_api(platform);
  const h2_audio_pcm_format_t audio_format = {
      .sample_rate_hz = 48000u,
      .frame_samples_per_channel = 4u,
      .channels = 1u,
      .sample_format = H2_AUDIO_SAMPLE_S16LE,
  };
  const h2_audio_track_config_t audio_track_config = {
      .name = "web-test",
      .format = audio_format,
      .volume_factor_milli = 1000u,
      .buffer_frames = 1u,
  };
  int16_t audio_samples[4] = {0, 100, -100, 0};
  h2_audio_frame_t audio_frame = h2_audio_frame_for_buffer(
      audio_samples, sizeof(audio_samples), audio_format);
  audio_frame.bytes = sizeof(audio_samples);
  h2_pal_audio_track_t *audio_track = NULL;
  if (h2_pal_audio_start_speaker(audio) != H2_AUDIO_OK ||
      h2_pal_audio_create_track(audio, &audio_track_config, &audio_track) !=
          H2_AUDIO_OK ||
      h2_pal_audio_track_write(audio_track, &audio_frame, 1000u) !=
          H2_AUDIO_OK ||
      h2_pal_audio_stop_speaker(audio) != H2_AUDIO_OK ||
      ((int)h2_web_main_call(h2_web_test_audio_stopped_sources, NULL).i32) !=
          1 ||
      ((int)h2_web_main_call(h2_web_test_audio_active_sources, NULL).i32) != 0 ||
      // A stopped speaker holds writes, like a device mixer queue, and
      // plays them when it starts again.
      h2_pal_audio_track_write(audio_track, &audio_frame, 1000u) !=
          H2_AUDIO_OK ||
      ((int)h2_web_main_call(h2_web_test_audio_active_sources, NULL).i32) != 0 ||
      h2_pal_audio_start_speaker(audio) != H2_AUDIO_OK ||
      ((int)h2_web_main_call(h2_web_test_audio_active_sources, NULL).i32) != 1 ||
      h2_pal_audio_stop_speaker(audio) != H2_AUDIO_OK ||
      ((int)h2_web_main_call(h2_web_test_audio_stopped_sources, NULL).i32) != 2 ||
      h2_pal_audio_track_close(audio_track) != H2_AUDIO_OK) {
    return 105;
  }
  {
    // A stopped track holds up to its queue capacity (8 frames); the next
    // write waits for room and returns WOULD_BLOCK at its timeout.
    static int16_t held_samples[960];
    const h2_audio_pcm_format_t held_format = {
        .sample_rate_hz = 16000u,
        .frame_samples_per_channel = 960u,
        .channels = 1u,
        .sample_format = H2_AUDIO_SAMPLE_S16LE,
    };
    const h2_audio_track_config_t held_config = {
        .name = "web-test-held",
        .format = held_format,
        .volume_factor_milli = 1000u,
        .buffer_frames = 8u,
    };
    h2_audio_frame_t held_frame = h2_audio_frame_for_buffer(
        held_samples, sizeof(held_samples), held_format);
    held_frame.bytes = sizeof(held_samples);
    h2_pal_audio_track_t *held_track = NULL;
    const int active =
        ((int)h2_web_main_call(h2_web_test_audio_active_sources, NULL).i32);
    if (h2_pal_audio_create_track(audio, &held_config, &held_track) !=
        H2_AUDIO_OK)
      return 111;
    for (int frame = 0; frame < 8; ++frame) {
      if (h2_pal_audio_track_write(held_track, &held_frame, 0u) !=
          H2_AUDIO_OK)
        return 112;
    }
    if (h2_pal_audio_track_write(held_track, &held_frame, 0u) !=
            H2_AUDIO_ERR_WOULD_BLOCK ||
        ((int)h2_web_main_call(h2_web_test_audio_active_sources, NULL).i32) != active ||
        h2_pal_audio_start_speaker(audio) != H2_AUDIO_OK ||
        ((int)h2_web_main_call(h2_web_test_audio_active_sources, NULL).i32) != active + 8 ||
        h2_pal_audio_stop_speaker(audio) != H2_AUDIO_OK ||
        h2_pal_audio_track_close(held_track) != H2_AUDIO_OK)
      return 113;
  }
  h2_web_webrtc_test_t webrtc_test = {0};
  const h2_pal_webrtc_api_t *webrtc = h2_web_platform_webrtc_api(platform);
  /* clang-format off */
MAIN_THREAD_EM_ASM({ globalThis.h2FakeCreateMedia(1); });
/* clang-format on */
  h2_pal_webrtc_track_t caller_track = {.native_handle = (void *)(uintptr_t)1u};
  h2_pal_webrtc_track_t *webrtc_track = &caller_track;
  webrtc_test.api = webrtc;
  h2_pal_webrtc_peer_t *webrtc_peer = NULL;
  h2_pal_webrtc_channel_t *webrtc_channel = NULL;
  const h2_pal_webrtc_ice_server_t ice_server = {
      .url = {.data = "stun:example.test", .len = 17u},
  };
  const h2_pal_webrtc_channel_config_t channel_config = {
      .label = {.data = "rpc", .len = 3u},
      .ordered = 1,
      .reliable = 1,
  };
  const h2_pal_webrtc_str_t answer = {
      .data = "fake-answer",
      .len = 11u,
  };
  const uint8_t opus[] = {0xf8u, 0xffu, 0xfeu};
  if (h2_pal_webrtc_peer_create(webrtc, &webrtc_peer) != H2_PAL_OK ||
      webrtc_track == NULL ||
      h2_pal_webrtc_peer_set_track(webrtc, webrtc_peer, webrtc_track) !=
          H2_PAL_OK ||
      h2_pal_webrtc_peer_add_ice_server(webrtc, webrtc_peer, &ice_server) !=
          H2_PAL_OK ||
      h2_pal_webrtc_peer_create_data_channel(
          webrtc, webrtc_peer, &channel_config, &webrtc_channel) != H2_PAL_OK ||
      h2_pal_webrtc_peer_start_offer(webrtc, webrtc_peer) != H2_PAL_OK) {
    return 101;
  }
  h2_web_test_webrtc_drain(&webrtc_test, webrtc_peer);
  if (webrtc_test.local_sdp != 1 ||
      h2_pal_webrtc_peer_set_remote_sdp(
          webrtc, webrtc_peer, H2_PAL_WEBRTC_SDP_ANSWER, answer) != H2_PAL_OK ||
      /* clang-format off */
MAIN_THREAD_EM_ASM_INT({ return globalThis.h2FakeGetUserMediaCount || 0; })
/* clang-format on */ != 0 || /* clang-format off */
MAIN_THREAD_EM_ASM_INT({ return globalThis.h2FakeAudioPlayCount || 0; })
/* clang-format on */ != 1 ||
      h2_pal_webrtc_channel_send(webrtc, webrtc_channel,
                                 (const uint8_t *)"ping", 4u, 1) != H2_PAL_OK ||
      h2_pal_webrtc_peer_send_opus(webrtc, webrtc_peer, opus, sizeof(opus)) !=
          H2_PAL_ERR_UNSUPPORTED) {
    return 101;
  }
  h2_web_test_webrtc_drain(&webrtc_test, webrtc_peer);
  if (webrtc_test.connected != 1 || webrtc_test.channel_open != 1)
    return 101;
  h2_web_worker_sleep(0u);
  h2_web_test_webrtc_drain(&webrtc_test, webrtc_peer);
  if (webrtc_test.message != 1)
    return 102;
  if (h2_pal_webrtc_peer_unset_track(webrtc, webrtc_peer, webrtc_track) !=
          H2_PAL_OK ||
      !/* clang-format off */
MAIN_THREAD_EM_ASM_INT({
        return globalThis.h2FakeDetachResolved &&
                   !Module.h2WebRtcTracks.get(1)
                        .stream.getAudioTracks()[0]
                        .stopped &&
                   Module.h2WebRtcTracks.get(1).audio.srcObject === null;
      })
/* clang-format on */)
    return 103;
  h2_pal_webrtc_peer_close(webrtc, webrtc_peer);
  h2_pal_pref_namespace_t *prefs = NULL;
  char *stored_port = NULL;
  const uint8_t status_bytes[] = {1u, 2u, 3u, 4u};
  void *stored_status = NULL;
  size_t stored_status_size = 0u;
  if (h2_pal_pref_open(h2_web_platform_pref_api(platform), "web-test",
                       H2_PAL_PREF_OPEN_READ_WRITE, &prefs) != H2_PAL_OK ||
      prefs->set_string(prefs, "slot_01", "web-serial-1") != H2_PAL_OK ||
      prefs->set_blob(prefs, "slot_01_status", status_bytes,
                      sizeof(status_bytes)) != H2_PAL_OK ||
      prefs->commit(prefs) != H2_PAL_OK || prefs->close(prefs) != H2_PAL_OK ||
      h2_pal_pref_open(h2_web_platform_pref_api(platform), "web-test",
                       H2_PAL_PREF_OPEN_READ_WRITE, &prefs) != H2_PAL_OK ||
      prefs->get_string(prefs, h2_web_platform_mem_api(), "slot_01",
                        &stored_port) != H2_PAL_OK ||
      strcmp(stored_port, "web-serial-1") != 0 ||
      prefs->get_blob(prefs, h2_web_platform_mem_api(), "slot_01_status",
                      &stored_status, &stored_status_size) != H2_PAL_OK ||
      stored_status_size != sizeof(status_bytes) ||
      memcmp(stored_status, status_bytes, sizeof(status_bytes)) != 0) {
    return 4;
  }
  h2_pal_mem_free(h2_web_platform_mem_api(), stored_port);
  h2_pal_mem_free(h2_web_platform_mem_api(), stored_status);
  if (prefs->remove(prefs, "slot_01") != H2_PAL_OK ||
      prefs->remove(prefs, "slot_01_status") != H2_PAL_OK ||
      prefs->commit(prefs) != H2_PAL_OK || prefs->close(prefs) != H2_PAL_OK) {
    return 4;
  }
  int ran = 0;
  h2_pal_task_t *task = NULL;
  if (h2_pal_task_start(h2_web_platform_task_api(platform), NULL,
                        h2_web_test_task, &ran, &task) != H2_PAL_OK ||
      task == NULL) {
    return 5;
  }
  size_t resumed = 99u;
  if (h2_web_platform_pump(platform, 1u, &resumed) != H2_PAL_OK ||
      resumed != 0u) {
    return 6;
  }
  if (h2_pal_task_join(h2_web_platform_task_api(platform), task) != H2_PAL_OK ||
      ran != 1) {
    return 7;
  }
  if (h2_pal_touch_open(h2_web_platform_touch_api(platform)) != H2_PAL_OK ||
      h2_pal_touch_close(h2_web_platform_touch_api(platform)) != H2_PAL_OK) {
    return 8;
  }
  if (h2_web_platform_serial_request_port(platform) != H2_PAL_OK) {
    return 9;
  }
  h2_web_worker_sleep(0u);
  char port_id[H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN];
  if (wait_authorization(platform, port_id,
                                            sizeof(port_id)) != H2_PAL_OK ||
      strcmp(port_id, "web-serial-1") != 0) {
    return 10;
  }
  if (h2_web_platform_serial_forget_result(platform) !=
          H2_PAL_ERR_WOULD_BLOCK ||
      h2_web_platform_serial_forget_port(platform, "bogus") !=
          H2_PAL_ERR_INVALID_ARG ||
      h2_web_platform_serial_forget_port(platform, "web-serial-99") !=
          H2_PAL_OK) {
    return 40;
  }
  if (h2_web_platform_serial_forget_result(platform) != H2_PAL_ERR_NOT_FOUND)
    return 41;
  // Keep the Promise pending explicitly: UI microtasks may now finish before
  // the calling Worker is scheduled again.
  /* clang-format off */
MAIN_THREAD_EM_ASM({
    globalThis.h2SavedForget = h2FakeSerialPort.forget;
    h2FakeSerialPort.forget = () => new Promise(resolve => {
      globalThis.h2ReleaseForget = () => h2SavedForget.call(h2FakeSerialPort).then(resolve);
    });
  });
/* clang-format on */
  if (h2_web_platform_serial_forget_port(platform, "web-serial-1") !=
          H2_PAL_OK ||
      h2_web_platform_serial_forget_result(platform) !=
          H2_PAL_ERR_WOULD_BLOCK)
    return 41;
  const double forget_deadline = emscripten_get_now() + 3000.0;
  while (!h2_web_main_call(h2_web_test_forget_release_ready, NULL).i32 &&
         emscripten_get_now() < forget_deadline)
    h2_web_worker_sleep(1u);
  if (!h2_web_main_call(h2_web_test_forget_release_ready, NULL).i32)
    return 42;
  /* clang-format off */
MAIN_THREAD_EM_ASM({
    h2ReleaseForget();
    h2FakeSerialPort.forget = h2SavedForget;
  });
/* clang-format on */
  for (unsigned i = 0; i < 1000u && h2_web_platform_serial_forget_result(
                                        platform) == H2_PAL_ERR_WOULD_BLOCK;
       ++i)
    h2_web_worker_sleep(1u);
  if (h2_web_platform_serial_forget_result(platform) != H2_PAL_OK ||
      ((int)h2_web_main_call(h2_web_test_forget_count, NULL).i32) != 1)
    return 42;
  if (h2_web_platform_serial_request_port(platform) != H2_PAL_OK)
    return 43;
  h2_web_worker_sleep(0u);
  if (wait_authorization(platform, port_id,
                                            sizeof(port_id)) != H2_PAL_OK ||
      strcmp(port_id, "web-serial-2") != 0) {
    return 44;
  }
  memcpy(h2_web_test_serial_port, port_id, strlen(port_id) + 1u);
  h2_web_serial_test_t serial_test = {
      .platform = platform,
      .result = -1,
  };
  task = NULL;
  if (h2_pal_task_start(h2_web_platform_task_api(platform), NULL,
                        h2_web_test_serial, &serial_test, &task) != H2_PAL_OK) {
    return 11;
  }
  int joined = 0;
  for (int iteration = 0; iteration < 32 && !joined; ++iteration) {
    if (h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK) {
      return 12;
    }
    if (h2_pal_task_join(h2_web_platform_task_api(platform), task) ==
        H2_PAL_OK) {
      joined = 1;
      break;
    }
    h2_web_worker_sleep(0u);
  }
  if (!joined || serial_test.result != 0) {
    return 13 + serial_test.result;
  }

  h2_web_serial_read_test_t read_edges[] = {
      {platform, 4, H2_PAL_ERR_TIMEOUT, -1},
      {platform, 5, H2_PAL_ERR_CLOSED, -1},
      {platform, 6, H2_PAL_OK, -1},
  };
  for (size_t index = 0u; index < sizeof(read_edges) / sizeof(read_edges[0]);
       ++index) {
    if (!h2_web_test_run_task(platform, h2_web_test_serial_read_edge,
                              &read_edges[index]) ||
        read_edges[index].result != 0) {
      return 17 + (int)index;
    }
  }

  h2_web_serial_read_test_t write_timeout = {
      .platform = platform,
      .result = -1,
  };
  if (!h2_web_test_run_task(platform, h2_web_test_serial_write_timeout,
                            &write_timeout) ||
      write_timeout.result != 0) {
    return 20;
  }

  h2_web_serial_read_test_t read_timeout_recovery = {
      .platform = platform,
      .result = -1,
  };
  if (!h2_web_test_run_task(platform, h2_web_test_serial_read_timeout_recovery,
                            &read_timeout_recovery) ||
      read_timeout_recovery.result != 0) {
    return 100;
  }

  h2_web_serial_read_test_t partial_read = {
      .platform = platform,
      .result = -1,
  };
  if (!h2_web_test_run_task(platform, h2_web_test_serial_partial_read,
                            &partial_read) ||
      partial_read.result != 0) {
    return 21;
  }
  h2_web_serial_read_test_t unplug_write = {
      .platform = platform,
      .result = -1,
  };
  if (!h2_web_test_run_task(platform, h2_web_test_serial_write_unplug,
                            &unplug_write) ||
      unplug_write.result != 0) {
    return 22;
  }
  h2_web_serial_read_test_t open_failures[] = {
      {platform, 10, H2_PAL_ERR_UNAVAILABLE, -1},
      {platform, 11, H2_PAL_ERR_UNAVAILABLE, -1},
  };
  for (size_t index = 0u;
       index < sizeof(open_failures) / sizeof(open_failures[0]); ++index) {
    if (!h2_web_test_run_task(platform, h2_web_test_serial_open_failure,
                              &open_failures[index]) ||
        open_failures[index].result != 0) {
      return 23 + (int)index;
    }
  }
  h2_web_serial_read_test_t close_wait = {
      .platform = platform,
      .result = -1,
  };
  if (!h2_web_test_run_task(platform, h2_web_test_serial_close_wait,
                            &close_wait) ||
      close_wait.result != 0) {
    return 25;
  }

  h2_web_timer_mutation_test_t timer_mutation = {
      .api = h2_web_platform_timer_api(platform),
  };
  const h2_pal_timer_config_t victim_config = {
      .period_ms = 60000u,
      .flags = H2_PAL_TIMER_FLAG_AUTO_START,
      .cb = h2_web_test_timer_victim,
      .cb_user = &timer_mutation,
  };
  const h2_pal_timer_config_t destroyer_config = {
      .period_ms = 1u,
      .flags = H2_PAL_TIMER_FLAG_AUTO_START,
      .cb = h2_web_test_timer_destroyer,
      .cb_user = &timer_mutation,
  };
  h2_pal_timer_t *destroyer = NULL;
  if (h2_pal_timer_create(timer_mutation.api, &victim_config,
                          &timer_mutation.victim) != H2_PAL_OK ||
      h2_pal_timer_create(timer_mutation.api, &destroyer_config, &destroyer) !=
          H2_PAL_OK) {
    return 26;
  }
  for (unsigned i = 0; !timer_mutation.done && i < 3000u; ++i)
    h2_web_worker_sleep(1u);
  if (!timer_mutation.done ||
      timer_mutation.callbacks != 1 || timer_mutation.result != 0) {
    return 27;
  }

  _Atomic int repeat_calls = 0;
  h2_pal_timer_t *repeating = NULL;
  const h2_pal_timer_config_t repeat_config = {
      .period_ms = 2u,
      .flags = H2_PAL_TIMER_FLAG_AUTO_START | H2_PAL_TIMER_FLAG_REPEAT,
      .cb = h2_web_test_timer_count,
      .cb_user = &repeat_calls,
  };
  if (h2_pal_timer_create(timer_mutation.api, &repeat_config, &repeating) !=
      H2_PAL_OK)
    return 28;
  for (unsigned i = 0; repeat_calls < 3 && i < 3000u; ++i)
    h2_web_worker_sleep(1u);
  if (repeat_calls < 3 ||
      h2_pal_timer_stop(timer_mutation.api, repeating) != H2_PAL_OK)
    return 29;
  int stopped_calls = repeat_calls;
  h2_web_worker_sleep(20u);
  if (repeat_calls != stopped_calls ||
      h2_pal_timer_reset(timer_mutation.api, repeating) != H2_PAL_OK)
    return 30;
  for (unsigned i = 0; repeat_calls == stopped_calls && i < 3000u; ++i)
    h2_web_worker_sleep(1u);
  if (repeat_calls == stopped_calls ||
      h2_pal_timer_destroy(timer_mutation.api, repeating) != H2_PAL_OK)
    return 31;

  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){1}});
  if (h2_web_platform_serial_request_port(platform) != H2_PAL_OK)
    return 32;
  h2_web_worker_sleep(0u);
  if (wait_authorization(
          platform, port_id, sizeof(port_id)) != H2_PAL_ERR_NOT_FOUND) {
    return 33;
  }
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){2}});
  if (h2_web_platform_serial_request_port(platform) != H2_PAL_OK ||
      wait_authorization(
          platform, port_id, sizeof(port_id)) != H2_PAL_ERR_UNSUPPORTED) {
    return 34;
  }
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){3}});
  if (h2_web_platform_serial_request_port(platform) != H2_PAL_OK)
    return 35;
  h2_web_platform_destroy(platform);
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  platform = h2_web_platform_create(&valid);
  if (platform == NULL)
    return 36;
  h2_web_worker_sleep(2u);
  if (h2_web_platform_serial_authorization(
          platform, port_id, sizeof(port_id)) != H2_PAL_ERR_WOULD_BLOCK) {
    return 37;
  }
  h2_web_platform_destroy(platform);
  platform = h2_web_platform_create(&valid);
  h2_web_serial_test_t shutdown_test = {
      .platform = platform,
      .result = -1,
  };
  task = NULL;
  /* clang-format off */
MAIN_THREAD_EM_ASM({ globalThis.h2FakePendingReadResolve = null; });
/* clang-format on */
  if (platform == NULL ||
      h2_pal_task_start(h2_web_platform_task_api(platform), NULL,
                        h2_web_test_serial_shutdown, &shutdown_test,
                        &task) != H2_PAL_OK ||
      h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK) {
    return 45;
  }
  // Wait until open has completed and the reader really is blocked. A single
  // event-loop turn does not guarantee the chained open promises have settled.
  for (int iteration = 0;
       iteration < 32 && !/* clang-format off */
MAIN_THREAD_EM_ASM_INT(
                             { return !!globalThis.h2FakePendingReadResolve; });
/* clang-format on */
       ++iteration) {
    h2_web_worker_sleep(1u);
    if (h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK)
      return 45;
  }
  if (!/* clang-format off */
MAIN_THREAD_EM_ASM_INT({ return !!globalThis.h2FakePendingReadResolve; })
/* clang-format on */)
    return 45;
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){12}});
  if (h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK ||
      h2_web_platform_serial_shutdown(platform) != H2_PAL_ERR_UNSUPPORTED ||
      !((int)h2_web_main_call(h2_web_test_close_rejected_before_cancel_settled,
                              NULL)
            .i32)) {
    return 46;
  }
  atomic_store(&shutdown_test.shutdown_checked, 1);
  joined = 0;
  const double shutdown_join_deadline = emscripten_get_now() + 3000.0;
  while (!joined && emscripten_get_now() < shutdown_join_deadline) {
    if (h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK)
      return 47;
    joined =
        h2_pal_task_join(h2_web_platform_task_api(platform), task) == H2_PAL_OK;
    if (!joined) h2_web_worker_sleep(1u);
  }
  if (!joined || shutdown_test.result != 0 ||
      h2_web_platform_serial_request_port(platform) !=
          H2_PAL_ERR_INVALID_STATE) {
    return 48;
  }
  (void)h2_web_main_call(h2_web_test_set_serial_mode,
                         (const void *[]){&(int){0}});
  h2_web_task_cancel_test_t task_cancel_test = {
      .platform = platform,
      .result = H2_PAL_ERR_INVALID_STATE,
  };
  task = NULL;
  if (h2_pal_task_start(h2_web_platform_task_api(platform), NULL,
                        h2_web_test_task_cancel, &task_cancel_test,
                        &task) != H2_PAL_OK ||
      h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK ||
      h2_web_platform_task_cancel(platform, task) != H2_PAL_OK ||
      h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK ||
      h2_pal_task_join(h2_web_platform_task_api(platform), task) != H2_PAL_OK ||
      task_cancel_test.result != H2_PAL_EXIT) {
    return 49;
  }
  h2_web_platform_destroy(platform);
  h2_web_auto_pump.platform = h2_web_platform_create(&valid);
  if (h2_web_auto_pump.platform == NULL ||
      h2_pal_task_start(h2_web_platform_task_api(h2_web_auto_pump.platform),
                        NULL, h2_web_test_auto_pump, &h2_web_auto_pump,
          &h2_web_auto_pump_task) != H2_PAL_OK ||
      h2_web_platform_pump(h2_web_auto_pump.platform, 1u, NULL) != H2_PAL_OK) {
    return 50;
  }
  emscripten_set_timeout(h2_web_test_auto_pump_verify, 5.0, NULL);
  emscripten_set_main_loop(h2_web_test_main_loop, 0, 1);
  return 38;
}

int main(void) {
  int result = run_tests();
  if (result != 0) {
    fprintf(stderr, "Web PAL test failed: %d\n", result);
    emscripten_force_exit(result);
  }
  return 0;
}
