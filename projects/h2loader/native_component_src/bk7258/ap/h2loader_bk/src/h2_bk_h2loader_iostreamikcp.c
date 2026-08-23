#include "h2_bk_h2loader.h"
#include "h2_bk_h2loader_internal.h"

#include "h2_bk_platform_core.h"
#include "h2_command.h"
#include "h2_iostreamikcp.h"
#include "h2_loader_app_client.h"
#include "h2_loader_command.h"
#include "h2_loader_status.h"
#include "os/os.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define H2_BK_SERIAL_BAUD_RATE 230400u
#define H2_BK_SERIAL_DEFAULT_WRITE_TIMEOUT_MS 5000u
#define H2_BK_SERIAL_MAX_FLUSH_TIMEOUT_MS 120000u
#define H2_BK_SERIAL_MAX_WAITSND 64u
#define H2_BK_SERIAL_POLL_INTERVAL_MS 10u
#define H2_BK_SERIAL_PHYSICAL_READ_SIZE 512u
#define H2_BK_SERIAL_SEGMENT_TIMEOUT_MS 60u
#define H2_BK_SERIAL_RECEIVE_WINDOW 20u
typedef struct h2_bk_serial_transport {
  h2_iostreamikcp_io_t physical_io;
  h2_iostreamikcp_filter_t filter;
  h2_iostreamikcp_t *stream;
  const h2_pal_mem_api_t *allocator;
  uint32_t conv;
  uint32_t pending_conv;
  uint32_t write_timeout_ms;
  int replacement_pending;
  const atomic_bool *stop_requested;
} h2_bk_serial_transport_t;

typedef struct h2_bk_app_serial {
  h2_bk_serial_transport_t transport;
  h2_command_io_api_t io;
  h2_loader_app_client_t client;
  atomic_bool stop_requested;
} h2_bk_app_serial_t;

typedef struct h2_bk_loader_serial {
  h2_bk_serial_transport_t transport;
  h2_loader_command_t *command;
  h2_loader_command_config_t command_config;
  const h2_pal_mem_api_t *allocator;
  const h2_pal_task_api_t *task_api;
  h2_pal_task_t *task;
  atomic_bool stop_requested;
} h2_bk_loader_serial_t;

static h2_bk_app_serial_t s_app_serial;
static h2_bk_loader_serial_t s_loader_serial;
static int s_app_serial_started;
static int s_loader_serial_started;

static int transport_stop_requested(const h2_bk_serial_transport_t *transport) {
  return transport != NULL && transport->stop_requested != NULL &&
         atomic_load_explicit(transport->stop_requested, memory_order_acquire);
}

static uint32_t transport_now_ms(void *user) {
  (void)user;
  return (uint32_t)rtos_get_time();
}

static void write_le32(uint8_t out[4], uint32_t value) {
  out[0] = (uint8_t)(value & 0xffu);
  out[1] = (uint8_t)((value >> 8u) & 0xffu);
  out[2] = (uint8_t)((value >> 16u) & 0xffu);
  out[3] = (uint8_t)((value >> 24u) & 0xffu);
}

static h2_pal_result_t send_control(h2_bk_serial_transport_t *transport,
                                    uint8_t flags, uint32_t conv) {
  uint8_t payload[H2_IOSTREAMIKCP_SESSION_CONTROL_PAYLOAD_LEN];
  uint8_t encoded[H2_IOSTREAMIKCP_FRAME_HEADER_LEN +
                  H2_IOSTREAMIKCP_SESSION_CONTROL_PAYLOAD_LEN];
  size_t encoded_len = 0u;
  size_t written = 0u;
  write_le32(payload, conv);
  h2_iostreamikcp_frame_t frame = {
      .flags = flags,
      .conv = conv,
      .payload = payload,
      .payload_len = sizeof(payload),
  };
  h2_pal_result_t rc = h2_iostreamikcp_frame_encode(
      &frame, encoded, sizeof(encoded), &encoded_len);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  rc = transport->physical_io.write(transport->physical_io.user, encoded,
                                    encoded_len, &written, 1000u);
  if (rc != H2_PAL_OK || written != encoded_len) {
    return rc != H2_PAL_OK ? rc : H2_PAL_ERR_IO;
  }
  return transport->physical_io.flush != NULL
             ? transport->physical_io.flush(transport->physical_io.user)
             : H2_PAL_OK;
}

static h2_pal_result_t on_frame(void *user,
                                const h2_iostreamikcp_frame_t *frame) {
  h2_bk_serial_transport_t *transport = user;
  if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN) {
    if (frame->conv == 0u) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    if (frame->conv == transport->conv && transport->stream != NULL) {
      return send_control(transport, H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK,
                          frame->conv);
    }
    transport->pending_conv = frame->conv;
    transport->replacement_pending = transport->stream != NULL;
    return H2_PAL_OK;
  }
  if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_DATA &&
      transport->stream != NULL && frame->conv == transport->conv) {
    return h2_iostreamikcp_input_frame(transport->stream, frame);
  }
  return H2_PAL_OK;
}

static h2_pal_result_t poll_physical(h2_bk_serial_transport_t *transport,
                                     uint32_t timeout_ms) {
  uint8_t buffer[H2_BK_SERIAL_PHYSICAL_READ_SIZE];
  size_t count = 0u;
  uint32_t read_timeout_ms = timeout_ms;
  if (transport_stop_requested(transport)) {
    return H2_PAL_ERR_CLOSED;
  }
  if (transport->stream != NULL &&
      read_timeout_ms > H2_BK_SERIAL_POLL_INTERVAL_MS) {
    read_timeout_ms = H2_BK_SERIAL_POLL_INTERVAL_MS;
  }
  h2_pal_result_t read_rc =
      transport->physical_io.read(transport->physical_io.user, buffer,
                                  sizeof(buffer), &count, read_timeout_ms);
  if (read_rc != H2_PAL_OK && read_rc != H2_PAL_ERR_TIMEOUT &&
      read_rc != H2_PAL_ERR_WOULD_BLOCK) {
    return read_rc;
  }
  h2_pal_result_t rc = H2_PAL_OK;
  if (count != 0u) {
    rc = h2_iostreamikcp_filter_input(&transport->filter, buffer, count,
                                      on_frame, transport);
  }
  if (rc == H2_PAL_OK && transport->stream != NULL) {
    rc = h2_iostreamikcp_update(transport->stream, transport_now_ms(NULL));
  }
  return rc != H2_PAL_OK ? rc : read_rc;
}

static h2_pal_result_t command_read(void *user, void *buffer, size_t len,
                                    size_t *out_read, uint32_t timeout_ms) {
  h2_bk_serial_transport_t *transport = user;
  if (transport == NULL || buffer == NULL || out_read == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (transport->stream == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  uint32_t started = transport_now_ms(NULL);
  *out_read = 0u;
  for (;;) {
    if (transport_stop_requested(transport)) {
      return H2_PAL_ERR_CLOSED;
    }
    if (transport->replacement_pending) {
      return H2_PAL_ERR_CLOSED;
    }
    h2_pal_result_t rc =
        h2_iostreamikcp_read(transport->stream, buffer, len, out_read);
    if (rc == H2_PAL_OK && *out_read != 0u) {
      /* poll_physical() updates KCP after every input frame, and the UART
       * output path already waits for CP to finish the physical frame. Do
       * not issue an extra mailbox flush here: BK's mailbox tx-over signal
       * can remain false for its full timeout and would stall every command
       * read even though these bytes were already delivered successfully. */
      return H2_PAL_OK;
    }
    if (rc != H2_PAL_ERR_WOULD_BLOCK) {
      return rc;
    }
    uint32_t elapsed = transport_now_ms(NULL) - started;
    if (elapsed >= timeout_ms) {
      return H2_PAL_ERR_TIMEOUT;
    }
    rc = poll_physical(transport, timeout_ms - elapsed);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
        rc != H2_PAL_ERR_WOULD_BLOCK) {
      return rc;
    }
  }
}

static h2_pal_result_t command_write(void *user, const void *buffer, size_t len,
                                     size_t *out_written, uint32_t timeout_ms) {
  h2_bk_serial_transport_t *transport = user;
  if (transport == NULL || buffer == NULL || out_written == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_written = 0u;
  if (transport_stop_requested(transport)) {
    return H2_PAL_ERR_CLOSED;
  }
  if (transport->stream == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (transport->replacement_pending) {
    return H2_PAL_ERR_CLOSED;
  }
  transport->write_timeout_ms =
      timeout_ms == 0u ? H2_BK_SERIAL_DEFAULT_WRITE_TIMEOUT_MS : timeout_ms;
  uint32_t started = transport_now_ms(NULL);
  for (;;) {
    h2_iostreamikcp_stats_t stats;
    h2_pal_result_t rc = h2_iostreamikcp_get_stats(transport->stream, &stats);
    if (rc != H2_PAL_OK) {
      return rc;
    }
    if (stats.waitsnd < H2_BK_SERIAL_MAX_WAITSND) {
      break;
    }
    if (transport->replacement_pending) {
      return H2_PAL_ERR_CLOSED;
    }
    if (transport_stop_requested(transport)) {
      return H2_PAL_ERR_CLOSED;
    }
    uint32_t elapsed = transport_now_ms(NULL) - started;
    if (elapsed >= transport->write_timeout_ms) {
      return H2_PAL_ERR_TIMEOUT;
    }
    uint32_t remaining = transport->write_timeout_ms - elapsed;
    uint32_t poll_ms = remaining < H2_BK_SERIAL_POLL_INTERVAL_MS
                           ? remaining
                           : H2_BK_SERIAL_POLL_INTERVAL_MS;
    rc = poll_physical(transport, poll_ms);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
        rc != H2_PAL_ERR_WOULD_BLOCK) {
      return rc;
    }
  }
  h2_pal_result_t rc = h2_iostreamikcp_write(transport->stream, buffer, len);
  *out_written = rc == H2_PAL_OK ? len : 0u;
  if (rc == H2_PAL_OK) {
    h2_pal_result_t poll_rc = poll_physical(transport, 0u);
    if (poll_rc != H2_PAL_OK && poll_rc != H2_PAL_ERR_TIMEOUT &&
        poll_rc != H2_PAL_ERR_WOULD_BLOCK) {
      return poll_rc;
    }
  }
  return rc;
}

static h2_pal_result_t command_flush(void *user) {
  h2_bk_serial_transport_t *transport = user;
  if (transport == NULL || transport->stream == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (transport_stop_requested(transport)) {
    return H2_PAL_ERR_CLOSED;
  }
  if (transport->replacement_pending) {
    return H2_PAL_ERR_CLOSED;
  }
  h2_iostreamikcp_stats_t stats = {0};
  h2_pal_result_t rc = h2_iostreamikcp_flush(transport->stream);
  if (rc == H2_PAL_OK) {
    rc = h2_iostreamikcp_get_stats(transport->stream, &stats);
  }
  uint64_t estimate = (uint64_t)stats.waitsnd * H2_BK_SERIAL_SEGMENT_TIMEOUT_MS;
  uint32_t timeout_ms = transport->write_timeout_ms;
  if (estimate > timeout_ms) {
    timeout_ms = estimate > H2_BK_SERIAL_MAX_FLUSH_TIMEOUT_MS
                     ? H2_BK_SERIAL_MAX_FLUSH_TIMEOUT_MS
                     : (uint32_t)estimate;
  }
  uint32_t started = transport_now_ms(NULL);
  while (rc == H2_PAL_OK) {
    rc = h2_iostreamikcp_get_stats(transport->stream, &stats);
    if (rc != H2_PAL_OK || stats.waitsnd == 0u) {
      break;
    }
    if (transport->replacement_pending) {
      return H2_PAL_ERR_CLOSED;
    }
    if (transport_stop_requested(transport)) {
      return H2_PAL_ERR_CLOSED;
    }
    uint32_t elapsed = transport_now_ms(NULL) - started;
    if (elapsed >= timeout_ms) {
      return H2_PAL_ERR_TIMEOUT;
    }
    uint32_t remaining = timeout_ms - elapsed;
    uint32_t poll_ms = remaining < H2_BK_SERIAL_POLL_INTERVAL_MS
                           ? remaining
                           : H2_BK_SERIAL_POLL_INTERVAL_MS;
    rc = poll_physical(transport, poll_ms);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
      rc = h2_iostreamikcp_update(transport->stream, transport_now_ms(NULL));
    }
  }
  return rc;
}

static const h2_command_io_vtable_t s_command_io_vtable = {
    .read = command_read,
    .write = command_write,
    .flush = command_flush,
};

static h2_pal_result_t transport_init(h2_bk_serial_transport_t *transport,
                                      const h2_pal_mem_api_t *allocator,
                                      const atomic_bool *stop_requested) {
  if (transport == NULL || allocator == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(transport, 0, sizeof(*transport));
  const h2_pal_uart_io_stream_api_t *uart = h2_bk_platform_uart_io_stream_api();
  const h2_pal_uart_io_stream_config_t uart_config = {
      .baud_rate = H2_BK_SERIAL_BAUD_RATE,
      .data_bits = 8u,
      .stop_bits = 1u,
      .parity = H2_PAL_UART_PARITY_NONE,
      .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
      .rx_buffer_size = 4096u,
      .tx_buffer_size = 2048u,
  };
  h2_pal_result_t rc = h2_pal_uart_io_stream_configure(uart, &uart_config);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  transport->physical_io = h2_iostreamikcp_io_from_uart(uart);
  transport->allocator = allocator;
  transport->stop_requested = stop_requested;
  transport->write_timeout_ms = H2_BK_SERIAL_DEFAULT_WRITE_TIMEOUT_MS;
  h2_iostreamikcp_filter_init(&transport->filter);
  return H2_PAL_OK;
}

static void transport_deinit(h2_bk_serial_transport_t *transport) {
  if (transport != NULL) {
    h2_iostreamikcp_close(transport->stream);
    memset(transport, 0, sizeof(*transport));
  }
  h2_bk_platform_uart_io_stream_deinit();
}

static h2_command_io_api_t transport_io(h2_bk_serial_transport_t *transport) {
  const h2_command_io_api_t io = {
      .user = transport,
      .vtable = &s_command_io_vtable,
  };
  return io;
}

static h2_pal_result_t activate_pending(h2_bk_serial_transport_t *transport) {
  if (transport == NULL || transport->pending_conv == 0u) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  uint32_t conv = transport->pending_conv;
  h2_iostreamikcp_close(transport->stream);
  transport->stream = NULL;
  const h2_iostreamikcp_config_t config = {
      .io = transport->physical_io,
      .allocator = transport->allocator,
      .now_ms = transport_now_ms,
      .conv = conv,
      .mtu = H2_IOSTREAMIKCP_DEFAULT_MTU,
      .rx_buffer_size = 4096u,
      .receive_window = H2_BK_SERIAL_RECEIVE_WINDOW,
      .write_timeout_ms = 1000u,
  };
  h2_pal_result_t rc = h2_iostreamikcp_open(&config, &transport->stream);
  if (rc != H2_PAL_OK) {
    transport->pending_conv = 0u;
    transport->replacement_pending = 0;
    transport->conv = 0u;
    return rc;
  }
  transport->conv = conv;
  transport->pending_conv = 0u;
  transport->replacement_pending = 0;
  rc = send_control(transport, H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK, conv);
  if (rc != H2_PAL_OK) {
    h2_iostreamikcp_close(transport->stream);
    transport->stream = NULL;
    transport->conv = 0u;
  }
  return rc;
}

static int
serve_loader_iostreamikcp(h2_bk_serial_transport_t *transport,
                          h2_loader_command_t *command,
                          const h2_loader_command_config_t *command_config) {
  if (transport == NULL || command == NULL || command_config == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t rc = H2_PAL_OK;
  h2_loader_command_config_t serial_config = *command_config;
  serial_config.io = transport_io(transport);
  while (!transport_stop_requested(transport)) {
    if (transport->pending_conv != 0u) {
      rc = h2_loader_command_init(command, &serial_config);
      if (rc == H2_PAL_OK) {
        rc = activate_pending(transport);
      }
      if (rc != H2_PAL_OK) {
        printf("H2_BK_SERIAL_ERROR reason=session_activate code=%d\r\n", rc);
      }
      continue;
    }
    if (transport->stream == NULL) {
      rc = poll_physical(transport, 50u);
    } else {
      rc = h2_loader_command_poll(command, 50u);
    }
    if (rc == H2_PAL_OK || rc == H2_PAL_ERR_TIMEOUT ||
        rc == H2_PAL_ERR_WOULD_BLOCK ||
        (rc == H2_PAL_ERR_CLOSED && transport->replacement_pending)) {
      continue;
    }
    printf("H2_BK_SERIAL_ERROR reason=command code=%d\r\n", rc);
  }
  int stopped = transport_stop_requested(transport);
  transport_deinit(transport);
  return stopped ? H2_PAL_OK : rc;
}

static void loader_serial_task(void *user) {
  h2_bk_loader_serial_t *state = user;
  int rc = serve_loader_iostreamikcp(&state->transport, state->command,
                                     &state->command_config);
  if (rc != H2_PAL_OK) {
    printf("H2_BK_SERIAL_ERROR reason=service_exit code=%d\r\n", rc);
  }
}

int h2_bk_h2loader_start_loader_iostreamikcp(
    h2_runtime_t *runtime, h2_loader_command_t *command,
    const h2_loader_command_config_t *command_config) {
  if (runtime == NULL || runtime->task == NULL || runtime->mem == NULL ||
      command == NULL || command_config == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (s_loader_serial_started) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_bk_loader_serial_t *state = &s_loader_serial;
  memset(state, 0, sizeof(*state));
  state->command = command;
  state->command_config = *command_config;
  state->allocator = runtime->mem;
  state->task_api = runtime->task;
  atomic_init(&state->stop_requested, false);
  int rc = transport_init(&state->transport, state->allocator,
                          &state->stop_requested);
  if (rc != H2_PAL_OK) {
    memset(state, 0, sizeof(*state));
    return rc;
  }
  const h2_pal_task_options_t options = {
      .name = "h2loader/appcmd",
      .min_stack_size = H2_BK_H2LOADER_LOADER_COMMAND_STACK_SIZE,
  };
  rc = h2_pal_task_start(runtime->task, &options, loader_serial_task, state,
                        &state->task);
  if (rc != H2_PAL_OK) {
    transport_deinit(&state->transport);
    memset(state, 0, sizeof(*state));
  } else {
    s_loader_serial_started = 1;
  }
  return rc;
}

int h2_bk_h2loader_stop_loader_iostreamikcp(void) {
  if (!s_loader_serial_started) {
    return H2_PAL_OK;
  }
  atomic_store_explicit(&s_loader_serial.stop_requested, true,
                        memory_order_release);
  int rc = h2_pal_task_join(s_loader_serial.task_api, s_loader_serial.task);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  memset(&s_loader_serial, 0, sizeof(s_loader_serial));
  s_loader_serial_started = 0;
  return H2_PAL_OK;
}

static int app_read_byte(void *user, uint32_t timeout_ms) {
  h2_bk_app_serial_t *state = user;
  uint8_t value = 0u;
  size_t count = 0u;
  if (state == NULL) {
    return EOF;
  }
  for (;;) {
    if (atomic_load_explicit(&state->stop_requested, memory_order_acquire)) {
      return H2_LOADER_APP_CLIENT_SESSION_CLOSED;
    }
    if (state->transport.pending_conv != 0u) {
      h2_pal_result_t rc = activate_pending(&state->transport);
      return rc == H2_PAL_OK ? H2_LOADER_APP_CLIENT_SESSION_RESET : EOF;
    }
    if (state->transport.stream == NULL) {
      h2_pal_result_t rc = poll_physical(&state->transport, timeout_ms);
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
          rc != H2_PAL_ERR_WOULD_BLOCK) {
        return EOF;
      }
      if (state->transport.pending_conv == 0u) {
        return EOF;
      }
      continue;
    }
    h2_pal_result_t rc = state->io.vtable->read(
        state->io.user, &value, sizeof(value), &count, timeout_ms);
    if (rc == H2_PAL_OK && count == sizeof(value)) {
      return value;
    }
    if (rc == H2_PAL_ERR_CLOSED && state->transport.replacement_pending) {
      continue;
    }
    return EOF;
  }
}

static int app_write(void *user, const char *data, size_t len) {
  h2_bk_app_serial_t *state = user;
  size_t offset = 0u;
  if (state == NULL || (data == NULL && len != 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  while (offset < len) {
    size_t written = 0u;
    h2_pal_result_t rc = state->io.vtable->write(
        state->io.user, data + offset, len - offset, &written,
        H2_BK_SERIAL_DEFAULT_WRITE_TIMEOUT_MS);
    if (rc != H2_PAL_OK) {
      return rc;
    }
    if (written == 0u || written > len - offset) {
      return H2_PAL_ERR_IO;
    }
    offset += written;
  }
  return state->io.vtable->flush(state->io.user);
}

static int arm_pending_app_rollback(const h2_pal_pref_api_t *pref) {
  h2_loader_status_t status;
  int rc = h2_loader_read_pref_status(pref, NULL, &status);
  if (rc != H2_PAL_OK ||
      status.install_state !=
          H2_LOADER_INSTALL_STATE_INSTALLED_PENDING_CONFIRM) {
    return rc;
  }
  return h2_bk_h2loader_prepare_pending_app_rollback();
}

int h2_bk_h2loader_start_app_iostreamikcp(h2_runtime_t *runtime,
                                          const char *active_name) {
  if (runtime == NULL || runtime->task == NULL || runtime->mem == NULL ||
      runtime->pref == NULL || active_name == NULL || active_name[0] == '\0') {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (s_app_serial_started) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_bk_app_serial_t *state = &s_app_serial;
  memset(state, 0, sizeof(*state));
  atomic_init(&state->stop_requested, false);
  int rc = arm_pending_app_rollback(runtime->pref);
  if (rc == H2_PAL_OK) {
    rc = h2_bk_h2loader_init_app_client(runtime, active_name, &state->client);
  }
  if (rc == H2_PAL_OK) {
    rc =
        transport_init(&state->transport, runtime->mem, &state->stop_requested);
  }
  if (rc != H2_PAL_OK) {
    memset(state, 0, sizeof(*state));
    return rc;
  }
  state->io = transport_io(&state->transport);
  const h2_loader_app_client_return_console_config_t console = {
      .client = &state->client,
      .task = runtime->task,
      .read_user = state,
      .read_byte = app_read_byte,
      .write_user = state,
      .write = app_write,
      .task_name = "h2loader/uartcmd",
      .stack_size = H2_BK_H2LOADER_APP_COMMAND_STACK_SIZE,
  };
  rc = h2_loader_app_client_start_return_console(&console);
  if (rc != H2_PAL_OK) {
    transport_deinit(&state->transport);
    memset(state, 0, sizeof(*state));
  } else {
    s_app_serial_started = 1;
  }
  return rc;
}

int h2_bk_h2loader_stop_app_iostreamikcp(void) {
  if (!s_app_serial_started) {
    return H2_PAL_OK;
  }
  atomic_store_explicit(&s_app_serial.stop_requested, true,
                        memory_order_release);
  int rc = h2_loader_app_client_stop_return_console(&s_app_serial.client);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  transport_deinit(&s_app_serial.transport);
  memset(&s_app_serial, 0, sizeof(s_app_serial));
  s_app_serial_started = 0;
  return H2_PAL_OK;
}
