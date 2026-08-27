#include "h2_web_platform.h"

#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

EM_JS(void, h2_web_test_set_serial_mode, (int mode), {
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

EM_JS(int, h2_web_test_forget_count, (),
      { return globalThis.h2FakeForgetCount || 0; });

EM_JS(int, h2_web_test_close_rejected_before_cancel_settled, (),
      { return globalThis.h2FakeCloseRejectedBeforeCancelSettled ? 1 : 0; });

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
    exit(38);
  }
  emscripten_cancel_main_loop();
}

typedef struct h2_web_serial_test {
  h2_web_platform_t *platform;
  int result;
} h2_web_serial_test_t;

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
      strcmp(info.port_id, "web-serial-1") != 0 ||
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
  h2_web_test_set_serial_mode(0);
  if (h2_pal_serial_host_open(serial, "web-serial-1", &config, &session) !=
          H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK) {
    return;
  }
  h2_web_test_set_serial_mode(test->mode);
  const h2_pal_result_t result = h2_pal_uart_io_stream_read(
      stream, bytes, sizeof(bytes), &transferred, 1u);
  h2_web_test_set_serial_mode(0);
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
  h2_web_test_set_serial_mode(0);
  if (h2_pal_serial_host_open(serial, "web-serial-1", &config, &session) !=
          H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK) {
    return;
  }
  h2_web_test_set_serial_mode(7);
  const h2_pal_result_t timeout_result = h2_pal_uart_io_stream_write(
      stream, bytes, sizeof(bytes), &transferred, 1u);
  h2_web_test_set_serial_mode(0);
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
  h2_web_test_set_serial_mode(0);
  if (h2_pal_serial_host_open(serial, "web-serial-1", &config, &session) !=
          H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK) {
    return;
  }
  h2_web_test_set_serial_mode(13);
  const h2_pal_result_t timeout_result = h2_pal_uart_io_stream_read(
      stream, bytes, sizeof(bytes), &transferred, 1u);
  const h2_pal_result_t recovery_result = h2_pal_uart_io_stream_read(
      stream, bytes, sizeof(bytes), &transferred, 100u);
  h2_web_test_set_serial_mode(0);
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
  h2_web_test_set_serial_mode(0);
  if (h2_pal_serial_host_open(serial, "web-serial-1", &config, &session) !=
          H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK) {
    return;
  }
  h2_web_test_set_serial_mode(8);
  const h2_pal_result_t first_result = h2_pal_uart_io_stream_read(
      stream, first, sizeof(first), &transferred, 100u);
  const h2_pal_result_t second_result = h2_pal_uart_io_stream_read(
      stream, second, sizeof(second), &transferred, 100u);
  h2_web_test_set_serial_mode(0);
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
  h2_web_test_set_serial_mode(0);
  if (h2_pal_serial_host_open(serial, "web-serial-1", &config, &session) !=
          H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK) {
    return;
  }
  h2_web_test_set_serial_mode(9);
  const h2_pal_result_t write_result = h2_pal_uart_io_stream_write(
      stream, bytes, sizeof(bytes), &transferred, 100u);
  h2_web_test_set_serial_mode(0);
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
  h2_web_test_set_serial_mode(test->mode);
  const h2_pal_result_t result =
      h2_pal_serial_host_open(serial, "web-serial-1", &config, &session);
  h2_web_test_set_serial_mode(0);
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
  h2_web_test_set_serial_mode(0);
  if (h2_pal_serial_host_open(serial, "web-serial-1", &config, &session) !=
          H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK) {
    return;
  }
  h2_web_test_set_serial_mode(4);
  const h2_pal_result_t read_result = h2_pal_uart_io_stream_read(
      stream, &byte, sizeof(byte), &transferred, 10000u);
  h2_web_test_set_serial_mode(0);
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
  h2_web_test_set_serial_mode(0);
  if (h2_pal_serial_host_open(serial, "web-serial-1", &config, &session) !=
          H2_PAL_OK ||
      h2_pal_serial_host_session_stream(serial, session, &stream) !=
          H2_PAL_OK) {
    return;
  }
  state.stream = stream;
  h2_web_test_set_serial_mode(4);
  if (h2_pal_task_start(h2_web_platform_task_api(test->platform), NULL,
                        h2_web_test_serial_blocked_read, &state,
                        &reader) != H2_PAL_OK ||
      h2_pal_time_sleep_ms(h2_web_platform_time_api(test->platform), 1u) !=
          H2_PAL_OK) {
    return;
  }
  const h2_pal_result_t close_result =
      h2_pal_serial_host_close(serial, &session);
  h2_web_test_set_serial_mode(0);
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
  int callbacks;
  int result;
} h2_web_timer_mutation_test_t;

static void h2_web_test_timer_victim(void *user, h2_pal_timer_t *timer) {
  (void)timer;
  h2_web_timer_mutation_test_t *test = user;
  test->result = 1;
}

static void h2_web_test_timer_count(void *user, h2_pal_timer_t *timer) {
  (void)timer;
  ++*(int *)user;
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
    emscripten_sleep(1u);
  }
  return 0;
}

int main(void) {
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
      h2_web_platform_serial_host_api(platform) == NULL ||
      h2_web_platform_webrtc_api(platform) == NULL) {
    return 4;
  }
  h2_web_webrtc_test_t webrtc_test = {0};
  const h2_pal_webrtc_callbacks_t webrtc_callbacks = {
      .user = &webrtc_test,
      .on_peer_state = h2_web_test_webrtc_peer_state,
      .on_local_sdp = h2_web_test_webrtc_local_sdp,
      .on_channel_state = h2_web_test_webrtc_channel_state,
      .on_channel_message = h2_web_test_webrtc_message,
  };
  const h2_pal_webrtc_api_t *webrtc = h2_web_platform_webrtc_api(platform);
  h2_pal_webrtc_track_t *webrtc_track =
      h2_web_platform_webrtc_audio_track(platform);
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
  if (h2_pal_webrtc_peer_create(webrtc, &webrtc_callbacks, &webrtc_peer) !=
          H2_PAL_OK ||
      webrtc_track == NULL ||
      h2_pal_webrtc_peer_set_media_track(webrtc, webrtc_peer, webrtc_track) !=
          H2_PAL_OK ||
      h2_pal_webrtc_peer_add_ice_server(webrtc, webrtc_peer, &ice_server) !=
          H2_PAL_OK ||
      h2_pal_webrtc_peer_create_data_channel(
          webrtc, webrtc_peer, &channel_config, &webrtc_channel) != H2_PAL_OK ||
      h2_pal_webrtc_peer_start_offer(webrtc, webrtc_peer) != H2_PAL_OK ||
      webrtc_test.local_sdp != 1 ||
      h2_pal_webrtc_peer_set_remote_sdp(
          webrtc, webrtc_peer, H2_PAL_WEBRTC_SDP_ANSWER, answer) != H2_PAL_OK ||
      webrtc_test.connected != 1 || webrtc_test.channel_open != 1 ||
      EM_ASM_INT({ return globalThis.h2FakeGetUserMediaCount || 0; }) != 1 ||
      EM_ASM_INT({ return globalThis.h2FakeAudioPlayCount || 0; }) != 1 ||
      h2_pal_webrtc_channel_send(webrtc, webrtc_channel,
                                 (const uint8_t *)"ping", 4u, 1) != H2_PAL_OK ||
      h2_pal_webrtc_peer_send_opus(webrtc, webrtc_peer, opus, sizeof(opus)) !=
          H2_PAL_ERR_UNSUPPORTED) {
    return 101;
  }
  emscripten_sleep(0u);
  if (webrtc_test.message != 1)
    return 102;
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
      resumed != 1u || ran != 1) {
    return 6;
  }
  if (h2_pal_task_join(h2_web_platform_task_api(platform), task) != H2_PAL_OK) {
    return 7;
  }
  if (h2_pal_touch_open(h2_web_platform_touch_api(platform)) != H2_PAL_OK ||
      h2_pal_touch_close(h2_web_platform_touch_api(platform)) != H2_PAL_OK) {
    return 8;
  }
  if (h2_web_platform_serial_request_port(platform) != H2_PAL_OK) {
    return 9;
  }
  emscripten_sleep(0u);
  char port_id[H2_PAL_SERIAL_HOST_PORT_ID_MAX_LEN];
  if (h2_web_platform_serial_authorization(platform, port_id,
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
  emscripten_sleep(0u);
  if (h2_web_platform_serial_forget_result(platform) != H2_PAL_ERR_NOT_FOUND ||
      h2_web_platform_serial_forget_port(platform, "web-serial-1") !=
          H2_PAL_OK ||
      h2_web_platform_serial_forget_result(platform) !=
          H2_PAL_ERR_WOULD_BLOCK) {
    return 41;
  }
  emscripten_sleep(0u);
  if (h2_web_platform_serial_forget_result(platform) != H2_PAL_OK ||
      h2_web_test_forget_count() != 1) {
    return 42;
  }
  if (h2_web_platform_serial_request_port(platform) != H2_PAL_OK)
    return 43;
  emscripten_sleep(0u);
  if (h2_web_platform_serial_authorization(platform, port_id,
                                            sizeof(port_id)) != H2_PAL_OK ||
      strcmp(port_id, "web-serial-2") != 0) {
    return 44;
  }
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
    emscripten_sleep(0u);
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
      .period_ms = 1u,
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
  emscripten_sleep(2u);
  if (h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK ||
      timer_mutation.callbacks != 1 || timer_mutation.result != 0) {
    return 27;
  }

  int repeat_calls = 0;
  h2_pal_timer_t *repeating = NULL;
  const h2_pal_timer_config_t repeat_config = {
      .period_ms = 1u,
      .flags = H2_PAL_TIMER_FLAG_AUTO_START | H2_PAL_TIMER_FLAG_REPEAT,
      .cb = h2_web_test_timer_count,
      .cb_user = &repeat_calls,
  };
  if (h2_pal_timer_create(timer_mutation.api, &repeat_config, &repeating) !=
      H2_PAL_OK) {
    return 27;
  }
  emscripten_sleep(2u);
  if (h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK ||
      repeat_calls != 1) {
    return 28;
  }
  emscripten_sleep(2u);
  if (h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK ||
      repeat_calls != 2 ||
      h2_pal_timer_stop(timer_mutation.api, repeating) != H2_PAL_OK) {
    return 29;
  }
  emscripten_sleep(2u);
  if (h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK ||
      repeat_calls != 2 ||
      h2_pal_timer_reset(timer_mutation.api, repeating) != H2_PAL_OK) {
    return 30;
  }
  emscripten_sleep(2u);
  if (h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK ||
      repeat_calls != 3 ||
      h2_pal_timer_destroy(timer_mutation.api, repeating) != H2_PAL_OK) {
    return 31;
  }

  h2_web_test_set_serial_mode(1);
  if (h2_web_platform_serial_request_port(platform) != H2_PAL_OK)
    return 32;
  emscripten_sleep(0u);
  if (h2_web_platform_serial_authorization(
          platform, port_id, sizeof(port_id)) != H2_PAL_ERR_NOT_FOUND) {
    return 33;
  }
  h2_web_test_set_serial_mode(2);
  if (h2_web_platform_serial_request_port(platform) != H2_PAL_OK ||
      h2_web_platform_serial_authorization(
          platform, port_id, sizeof(port_id)) != H2_PAL_ERR_UNSUPPORTED) {
    return 34;
  }
  h2_web_test_set_serial_mode(3);
  if (h2_web_platform_serial_request_port(platform) != H2_PAL_OK)
    return 35;
  h2_web_platform_destroy(platform);
  h2_web_test_set_serial_mode(0);
  platform = h2_web_platform_create(&valid);
  if (platform == NULL)
    return 36;
  emscripten_sleep(2u);
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
  if (platform == NULL ||
      h2_pal_task_start(h2_web_platform_task_api(platform), NULL,
                        h2_web_test_serial_shutdown, &shutdown_test,
                        &task) != H2_PAL_OK ||
      h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK) {
    return 38;
  }
  emscripten_sleep(0u);
  h2_web_test_set_serial_mode(12);
  if (h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK ||
      h2_web_platform_serial_shutdown(platform) != H2_PAL_ERR_UNSUPPORTED ||
      !h2_web_test_close_rejected_before_cancel_settled()) {
    return 38;
  }
  joined = 0;
  for (int iteration = 0; iteration < 8 && !joined; ++iteration) {
    if (h2_web_platform_pump(platform, 8u, NULL) != H2_PAL_OK)
      return 38;
    joined =
        h2_pal_task_join(h2_web_platform_task_api(platform), task) == H2_PAL_OK;
  }
  if (!joined || shutdown_test.result != 0 ||
      h2_web_platform_serial_request_port(platform) !=
          H2_PAL_ERR_INVALID_STATE) {
    return 38;
  }
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
    return 38;
  }
  h2_web_platform_destroy(platform);
  h2_web_auto_pump.platform = h2_web_platform_create(&valid);
  if (h2_web_auto_pump.platform == NULL ||
      h2_pal_task_start(h2_web_platform_task_api(h2_web_auto_pump.platform),
                        NULL, h2_web_test_auto_pump, &h2_web_auto_pump,
          &h2_web_auto_pump_task) != H2_PAL_OK ||
      h2_web_platform_pump(h2_web_auto_pump.platform, 1u, NULL) != H2_PAL_OK) {
    return 38;
  }
  emscripten_set_timeout(h2_web_test_auto_pump_verify, 5.0, NULL);
  emscripten_set_main_loop(h2_web_test_main_loop, 0, 1);
  return 38;
}
