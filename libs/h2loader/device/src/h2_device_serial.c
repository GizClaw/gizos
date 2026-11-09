#include "h2_device_serial.h"
#include "h2_loader_task_names.h"

#include <stdio.h>
#include <string.h>

#define H2_WRITE_TIMEOUT_MS 5000u
#define H2_MAX_FLUSH_TIMEOUT_MS 120000u
#define H2_MAX_WAITSND 64u
#define H2_PHYSICAL_POLL_MS 10u
#define H2_PHYSICAL_READ_SIZE 512u
#define H2_SEGMENT_TIMEOUT_MS 60u
#define H2_STREAM_RECEIVE_WINDOW 20u

h2_pal_result_t h2_device_serial_endpoint_configure(
    h2_device_serial_endpoint_t* endpoint, const h2_iostreamikcp_io_t* io) {
  if (endpoint == NULL || io == NULL || io->read == NULL || io->write == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (endpoint->configured || endpoint->initialization_started)
    return H2_PAL_ERR_INVALID_STATE;
  endpoint->io = *io;
  endpoint->configured = 1;
  return H2_PAL_OK;
}

h2_pal_result_t h2_device_serial_endpoint_start(
    h2_device_serial_endpoint_t* endpoint,
    const h2_iostreamikcp_io_t* default_io,
    h2_pal_result_t (*initialize)(void* user), void* user) {
  if (endpoint == NULL) return H2_PAL_ERR_INVALID_ARG;
  endpoint->initialization_started = 1;
  if (endpoint->configured || endpoint->io.read != NULL) return H2_PAL_OK;
  if (default_io == NULL) return H2_PAL_ERR_UNSUPPORTED;
  if (default_io->read == NULL || default_io->write == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = initialize != NULL ? initialize(user) : H2_PAL_OK;
  if (rc == H2_PAL_OK) endpoint->io = *default_io;
  return rc;
}

uint32_t h2_device_serial_now(h2_device_serial_t* self) {
  uint64_t now = 0u;
  (void)h2_pal_time_get_monotonic_ms(self->time, &now);
  return (uint32_t)now;
}
static uint32_t now_ms32(void* user) { return h2_device_serial_now(user); }
static int transport_stopped(const h2_device_serial_t* self) {
  return self->config.stop_requested != NULL &&
         h2_atomic_bool_load(self->config.stop_requested, H2_ATOMIC_ACQUIRE);
}
static void deadline_start(h2_device_serial_t* self, uint32_t timeout_ms) {
  self->io_started_ms = h2_device_serial_now(self);
  self->io_timeout_ms = timeout_ms;
  self->io_deadline_active = 1;
}
static uint32_t remaining_ms(h2_device_serial_t* self, uint32_t maximum) {
  if (!self->io_deadline_active) return maximum;
  uint32_t elapsed = h2_device_serial_now(self) - self->io_started_ms;
  uint32_t remaining =
      elapsed >= self->io_timeout_ms ? 0u : self->io_timeout_ms - elapsed;
  return remaining < maximum ? remaining : maximum;
}

static h2_pal_result_t transport_read(void* user, void* buffer, size_t len,
                                      size_t* out_read, uint32_t timeout_ms) {
  h2_device_serial_t* self = user;
  return self->physical_io.read(self->physical_io.user, buffer, len, out_read,
                                remaining_ms(self, timeout_ms));
}

/* One deadline spans every KCP frame and every partial physical write. */
static h2_pal_result_t transport_write(void* user, const void* buffer,
                                       size_t len, size_t* out_written,
                                       uint32_t timeout_ms) {
  h2_device_serial_t* self = user;
  const uint32_t started = h2_device_serial_now(self);
  *out_written = 0u;
  while (*out_written < len) {
    if (transport_stopped(self)) return H2_PAL_ERR_CLOSED;
    uint32_t elapsed = h2_device_serial_now(self) - started;
    if ((timeout_ms != 0u && elapsed >= timeout_ms) ||
        (self->io_deadline_active && self->io_timeout_ms != 0u &&
         h2_device_serial_now(self) - self->io_started_ms >=
             self->io_timeout_ms))
      return H2_PAL_ERR_TIMEOUT;
    uint32_t budget = elapsed >= timeout_ms ? 0u : timeout_ms - elapsed;
    size_t count = 0u;
    int rc = self->physical_io.write(
        self->physical_io.user, (const uint8_t*)buffer + *out_written,
        len - *out_written, &count, remaining_ms(self, budget));
    if (count > len - *out_written) return H2_PAL_ERR_IO;
    *out_written += count;
    if (rc != H2_PAL_OK) return rc;
    if (count == 0u) return H2_PAL_ERR_WOULD_BLOCK;
  }
  return H2_PAL_OK;
}
static h2_pal_result_t transport_flush(void* user) {
  h2_device_serial_t* self = user;
  return self->physical_io.flush != NULL
             ? self->physical_io.flush(self->physical_io.user)
             : H2_PAL_OK;
}
static void write_le32(uint8_t out[4], uint32_t value) {
  for (unsigned i = 0u; i < 4u; ++i) out[i] = (uint8_t)(value >> (8u * i));
}
static h2_pal_result_t send_control(h2_device_serial_t* self, uint8_t flags,
                                    uint32_t conv) {
  uint8_t payload[H2_IOSTREAMIKCP_SESSION_CONTROL_PAYLOAD_LEN];
  uint8_t encoded[H2_IOSTREAMIKCP_FRAME_HEADER_LEN + sizeof(payload)];
  size_t size = 0u, written = 0u;
  write_le32(payload, conv);
  const h2_iostreamikcp_frame_t frame = {
      .flags = flags,
      .conv = conv,
      .payload = payload,
      .payload_len = sizeof(payload),
  };
  int rc =
      h2_iostreamikcp_frame_encode(&frame, encoded, sizeof(encoded), &size);
  if (rc == H2_PAL_OK)
    rc = transport_write(self, encoded, size, &written, H2_WRITE_TIMEOUT_MS);
  if (rc == H2_PAL_OK && written != size) rc = H2_PAL_ERR_IO;
  return rc == H2_PAL_OK ? transport_flush(self) : rc;
}
static h2_pal_result_t on_frame(void* user,
                                const h2_iostreamikcp_frame_t* frame) {
  h2_device_serial_t* self = user;
  self->last_frame_ms = h2_device_serial_now(self);
  if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN) {
    if (frame->conv == 0u) return H2_PAL_ERR_INVALID_ARG;
    if (self->config.sessions_admitted != NULL) {
      if (!h2_atomic_bool_load(self->config.sessions_admitted,
                               H2_ATOMIC_ACQUIRE) &&
          (uint32_t)(h2_device_serial_now(self) - self->gate_started_ms) <
              self->config.admission_fallback_ms)
        return H2_PAL_OK;
      h2_atomic_bool_store(self->config.sessions_admitted, true,
                           H2_ATOMIC_RELEASE);
    }
    if (self->stream != NULL && frame->conv == self->conv)
      return send_control(self, H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK,
                          frame->conv);
    self->pending_conv = frame->conv;
    self->replacement_pending = self->stream != NULL;
  } else if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_CLOSE &&
             self->stream != NULL && frame->conv == self->conv) {
    int rc =
        send_control(self, H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK, frame->conv);
    if (rc == H2_PAL_OK) self->close_pending = self->replacement_pending = 1;
    return rc;
  } else if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_DATA &&
             self->stream != NULL && frame->conv == self->conv) {
    return h2_iostreamikcp_input_frame(self->stream, frame);
  }
  return H2_PAL_OK;
}

void h2_device_serial_deactivate(h2_device_serial_t* self) {
  if (self == NULL) return;
  h2_iostreamikcp_close(self->stream);
  self->stream = NULL;
  self->conv = self->pending_conv = self->last_frame_ms = 0u;
  self->replacement_pending = self->close_pending = 0;
  h2_iostreamikcp_filter_init(&self->filter);
}

static h2_pal_result_t poll_physical(h2_device_serial_t* self,
                                     uint32_t timeout_ms) {
  uint8_t buffer[H2_PHYSICAL_READ_SIZE];
  size_t count = 0u;
  if (transport_stopped(self)) return H2_PAL_ERR_CLOSED;
  if (self->stream != NULL && self->config.idle_timeout_ms != 0u &&
      h2_device_serial_now(self) - self->last_frame_ms >=
          self->config.idle_timeout_ms) {
    h2_device_serial_deactivate(self);
    return H2_PAL_ERR_CLOSED;
  }
  uint32_t read_timeout =
      self->stream != NULL && timeout_ms > H2_PHYSICAL_POLL_MS
          ? H2_PHYSICAL_POLL_MS
          : timeout_ms;
  int read_rc =
      transport_read(self, buffer, sizeof(buffer), &count, read_timeout);
  if (read_rc != H2_PAL_OK && read_rc != H2_PAL_ERR_TIMEOUT &&
      read_rc != H2_PAL_ERR_WOULD_BLOCK) {
    if (read_rc == H2_PAL_ERR_CLOSED) h2_device_serial_deactivate(self);
    return read_rc;
  }
  /* Resume complete buffered frames after a callback deadline, including when
   * the physical link supplies zero new bytes. */
  int rc = h2_iostreamikcp_filter_input(&self->filter, buffer, count, on_frame,
                                        self);
  if (rc == H2_PAL_OK && self->stream != NULL && !self->replacement_pending)
    rc = h2_iostreamikcp_update(self->stream, h2_device_serial_now(self));
  if (rc == H2_PAL_ERR_CLOSED) h2_device_serial_deactivate(self);
  return rc != H2_PAL_OK ? rc : read_rc;
}

static h2_pal_result_t command_read_impl(void* user, void* buffer, size_t len,
                                         size_t* out_read,
                                         uint32_t timeout_ms) {
  h2_device_serial_t* self = user;
  if (buffer == NULL || out_read == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_read = 0u;
  if (self->stream == NULL) return H2_PAL_ERR_INVALID_STATE;
  uint32_t started = h2_device_serial_now(self);
  for (;;) {
    if (transport_stopped(self) || self->replacement_pending)
      return H2_PAL_ERR_CLOSED;
    int rc = h2_iostreamikcp_read(self->stream, buffer, len, out_read);
    /* No physical flush on a successful read: mailbox-backed UART output may
     * have no separate tx-over event despite successful frame delivery. */
    if (rc != H2_PAL_ERR_WOULD_BLOCK) return rc;
    uint32_t elapsed = h2_device_serial_now(self) - started;
    if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    rc = poll_physical(self, timeout_ms - elapsed);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
        rc != H2_PAL_ERR_WOULD_BLOCK)
      return rc;
  }
}
static h2_pal_result_t command_read(void* user, void* buffer, size_t len,
                                    size_t* out_read, uint32_t timeout_ms) {
  h2_device_serial_t* self = user;
  if (self == NULL) return H2_PAL_ERR_INVALID_ARG;
  deadline_start(self, timeout_ms);
  int rc = command_read_impl(user, buffer, len, out_read, timeout_ms);
  self->io_deadline_active = 0;
  return rc;
}
static h2_pal_result_t command_write_impl(void* user, const void* buffer,
                                          size_t len, size_t* out_written,
                                          uint32_t timeout_ms) {
  h2_device_serial_t* self = user;
  if ((buffer == NULL && len != 0u) || out_written == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_written = 0u;
  if (transport_stopped(self) || self->replacement_pending)
    return H2_PAL_ERR_CLOSED;
  if (self->stream == NULL) return H2_PAL_ERR_INVALID_STATE;
  self->write_timeout_ms = timeout_ms;
  uint32_t started = h2_device_serial_now(self);
  for (;;) {
    h2_iostreamikcp_stats_t stats;
    int rc = h2_iostreamikcp_get_stats(self->stream, &stats);
    if (rc != H2_PAL_OK) return rc;
    if (stats.waitsnd < H2_MAX_WAITSND) break;
    if (transport_stopped(self) || self->replacement_pending)
      return H2_PAL_ERR_CLOSED;
    uint32_t elapsed = h2_device_serial_now(self) - started;
    if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    uint32_t remaining = timeout_ms - elapsed;
    rc = poll_physical(self, remaining < H2_PHYSICAL_POLL_MS
                                 ? remaining
                                 : H2_PHYSICAL_POLL_MS);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
        rc != H2_PAL_ERR_WOULD_BLOCK)
      return rc;
  }
  int rc = h2_iostreamikcp_write(self->stream, buffer, len);
  if (rc == H2_PAL_OK) {
    *out_written = len;
    rc = poll_physical(self,
                       remaining_ms(self, self->config.post_write_poll_ms));
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
      rc = H2_PAL_OK;
  }
  return rc;
}
static h2_pal_result_t command_write(void* user, const void* buffer, size_t len,
                                     size_t* out_written, uint32_t timeout_ms) {
  h2_device_serial_t* self = user;
  if (self == NULL) return H2_PAL_ERR_INVALID_ARG;
  deadline_start(self, timeout_ms);
  int rc = command_write_impl(user, buffer, len, out_written, timeout_ms);
  self->io_deadline_active = 0;
  return rc;
}
static uint32_t flush_budget(h2_device_serial_t* self, uint32_t waitsnd) {
  uint64_t estimate = (uint64_t)waitsnd * H2_SEGMENT_TIMEOUT_MS;
  uint32_t budget =
      estimate > self->write_timeout_ms
          ? (estimate > H2_MAX_FLUSH_TIMEOUT_MS ? H2_MAX_FLUSH_TIMEOUT_MS
                                                : (uint32_t)estimate)
          : self->write_timeout_ms;
  return budget > H2_MAX_FLUSH_TIMEOUT_MS ? H2_MAX_FLUSH_TIMEOUT_MS : budget;
}
static h2_pal_result_t command_flush(void* user) {
  h2_device_serial_t* self = user;
  if (self == NULL || self->stream == NULL) return H2_PAL_ERR_INVALID_STATE;
  if (transport_stopped(self) || self->replacement_pending)
    return H2_PAL_ERR_CLOSED;
  h2_iostreamikcp_stats_t stats = {0};
  int rc = h2_iostreamikcp_get_stats(self->stream, &stats);
  if (rc != H2_PAL_OK) return rc;
  uint32_t budget = flush_budget(self, stats.waitsnd);
  deadline_start(self, budget);
  rc = h2_iostreamikcp_flush(self->stream);
  while (rc == H2_PAL_OK) {
    rc = h2_iostreamikcp_get_stats(self->stream, &stats);
    if (rc != H2_PAL_OK || stats.waitsnd == 0u) break;
    if (transport_stopped(self) || self->replacement_pending) {
      rc = H2_PAL_ERR_CLOSED;
      break;
    }
    uint32_t remaining = remaining_ms(self, budget);
    if (remaining == 0u) {
      rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
    rc = poll_physical(self, remaining < H2_PHYSICAL_POLL_MS
                                 ? remaining
                                 : H2_PHYSICAL_POLL_MS);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK)
      rc = H2_PAL_OK;
  }
  self->io_deadline_active = 0;
  return rc;
}
static const h2_command_io_vtable_t command_io_vtable = {
    .read = command_read,
    .write = command_write,
    .flush = command_flush,
};
h2_command_io_api_t h2_device_serial_io(h2_device_serial_t* self) {
  return (h2_command_io_api_t){.user = self, .vtable = &command_io_vtable};
}
h2_pal_result_t h2_device_serial_init_with_config(
    h2_device_serial_t* self, const h2_device_serial_config_t* config) {
  if (self == NULL || config == NULL || config->allocator == NULL ||
      config->time == NULL || config->physical_io.read == NULL ||
      config->physical_io.write == NULL ||
      (config->rx_buffer_size != 0u &&
       (config->rx_buffer_size < H2_IOSTREAMIKCP_DEFAULT_MTU ||
        config->rx_buffer_size > 4096u)) ||
      (config->receive_window != 0u &&
       (config->receive_window < H2_IOSTREAMIKCP_MIN_RECEIVE_WINDOW ||
        config->receive_window > H2_IOSTREAMIKCP_MAX_RECEIVE_WINDOW)))
    return H2_PAL_ERR_INVALID_ARG;
  uint64_t now;
  int rc = h2_pal_time_get_monotonic_ms(config->time, &now);
  if (rc != H2_PAL_OK) return rc;
  const h2_device_serial_config_t saved = *config;
  memset(self, 0, sizeof(*self));
  self->config = saved;
  self->physical_io = saved.physical_io;
  self->allocator = saved.allocator;
  self->time = saved.time;
  self->receive_window = saved.receive_window != 0u ? saved.receive_window
                                                    : H2_STREAM_RECEIVE_WINDOW;
  self->write_timeout_ms = H2_WRITE_TIMEOUT_MS;
  self->gate_started_ms = (uint32_t)now;
  h2_iostreamikcp_filter_init(&self->filter);
  return H2_PAL_OK;
}
h2_pal_result_t h2_device_serial_init(h2_device_serial_t* self,
                                      const h2_pal_mem_api_t* allocator,
                                      const h2_pal_time_api_t* time,
                                      const h2_iostreamikcp_io_t* physical_io) {
  if (physical_io == NULL) return H2_PAL_ERR_INVALID_ARG;
  const h2_device_serial_config_t config = {
      .physical_io = *physical_io, .allocator = allocator, .time = time};
  return h2_device_serial_init_with_config(self, &config);
}
void h2_device_serial_deinit(h2_device_serial_t* self) {
  h2_device_serial_deactivate(self);
}
h2_pal_result_t h2_device_serial_poll_session(h2_device_serial_t* self,
                                              uint32_t timeout_ms) {
  if (self == NULL) return H2_PAL_ERR_INVALID_ARG;
  deadline_start(self, timeout_ms);
  int rc = poll_physical(self, timeout_ms);
  self->io_deadline_active = 0;
  return rc;
}
h2_pal_result_t h2_device_serial_activate_pending(h2_device_serial_t* self) {
  if (self == NULL || self->pending_conv == 0u) return H2_PAL_ERR_INVALID_STATE;
  uint32_t conv = self->pending_conv;
  h2_device_serial_deactivate(self);
  const h2_iostreamikcp_config_t config = {
      .io = {.user = self,
             .read = transport_read,
             .write = transport_write,
             .flush = transport_flush},
      .allocator = self->allocator,
      .now_ms = now_ms32,
      .time_user = self,
      .conv = conv,
      .mtu = H2_IOSTREAMIKCP_DEFAULT_MTU,
      .rx_buffer_size = self->config.rx_buffer_size != 0u
                            ? self->config.rx_buffer_size
                            : 4096u,
      .receive_window = self->receive_window,
      .write_timeout_ms = H2_WRITE_TIMEOUT_MS,
  };
  int rc = h2_iostreamikcp_open(&config, &self->stream);
  if (rc == H2_PAL_OK) {
    self->conv = conv;
    self->last_frame_ms = h2_device_serial_now(self);
    deadline_start(self, H2_WRITE_TIMEOUT_MS);
    rc = send_control(self, H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK, conv);
    self->io_deadline_active = 0;
  }
  if (rc != H2_PAL_OK) {
    /* Consume the failed OPEN and release its candidate. A retransmitted OPEN
     * may retry after memory recovers; do not spin on the same pending input.
     */
    h2_device_serial_deactivate(self);
    (void)h2_pal_time_sleep_ms(self->time, H2_PHYSICAL_POLL_MS);
  }
  return rc;
}
int h2_device_serial_has_session(const h2_device_serial_t* self) {
  return self != NULL && self->stream != NULL;
}
int h2_device_serial_replacement_pending(const h2_device_serial_t* self) {
  return self != NULL && self->replacement_pending;
}
int h2_device_serial_close_pending(const h2_device_serial_t* self) {
  return self != NULL && self->close_pending;
}

static int app_read_byte(void* user, uint32_t timeout_ms) {
  h2_device_app_serial_t* self = user;
  uint8_t value;
  size_t count;
  if (self == NULL) return EOF;
  for (;;) {
    if (transport_stopped(&self->transport))
      return H2_LOADER_APP_CLIENT_SESSION_CLOSED;
    if (self->transport.close_pending && self->transport.pending_conv == 0u) {
      h2_device_serial_deactivate(&self->transport);
      return H2_LOADER_APP_CLIENT_SESSION_RESET;
    }
    if (self->transport.pending_conv != 0u) {
      int rc = h2_device_serial_activate_pending(&self->transport);
      return rc == H2_PAL_OK ? H2_LOADER_APP_CLIENT_SESSION_RESET : EOF;
    }
    if (self->transport.stream == NULL) {
      int rc = h2_device_serial_poll_session(&self->transport, timeout_ms);
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
          rc != H2_PAL_ERR_WOULD_BLOCK)
        return EOF;
      if (self->transport.pending_conv == 0u) return EOF;
      continue;
    }
    int rc =
        self->io.vtable->read(self->io.user, &value, 1u, &count, timeout_ms);
    if (rc == H2_PAL_OK && count == 1u) return value;
    if (rc == H2_PAL_ERR_CLOSED && self->transport.replacement_pending)
      continue;
    if (rc == H2_PAL_ERR_CLOSED && !transport_stopped(&self->transport))
      return H2_LOADER_APP_CLIENT_SESSION_RESET;
    return EOF;
  }
}
static int app_write(void* user, const char* data, size_t len) {
  h2_device_app_serial_t* self = user;
  if (self == NULL || (data == NULL && len != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  size_t written = 0u;
  int rc = self->io.vtable->write(self->io.user, data, len, &written,
                                  H2_WRITE_TIMEOUT_MS);
  if (rc != H2_PAL_OK) return rc;
  if (written != len) return H2_PAL_ERR_IO;
  /* Body and line terminator are emitted separately. Flush at the boundary so
   * peers waiting for a complete command line can still send their KCP ACK. */
  return len != 0u && (data[len - 1u] == '\n' || data[len - 1u] == '\r')
             ? self->io.vtable->flush(self->io.user)
             : H2_PAL_OK;
}
int h2_device_app_serial_start_with_config(
    h2_device_app_serial_t* self, h2_loader_app_client_t* client,
    const h2_device_app_serial_config_t* config) {
  if (self == NULL || client == NULL || config == NULL ||
      config->task == NULL || config->transport.allocator == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (self->started) return H2_PAL_ERR_INVALID_STATE;
  if (self->initialized) {
    if (self->client != client || self->task != config->task ||
        self->transport.allocator != config->transport.allocator ||
        self->transport.time != config->transport.time ||
        self->transport.physical_io.user !=
            config->transport.physical_io.user ||
        self->transport.physical_io.read !=
            config->transport.physical_io.read ||
        self->transport.physical_io.write !=
            config->transport.physical_io.write ||
        self->transport.physical_io.flush !=
            config->transport.physical_io.flush)
      return H2_PAL_ERR_INVALID_STATE;
  } else {
    if (h2_atomic_bool_init(&self->stop_requested, false) != H2_ATOMIC_OK)
      return H2_PAL_ERR_NO_MEMORY;
    if (h2_atomic_bool_init(&self->sessions_admitted,
                            self->admission_requested != 0) != H2_ATOMIC_OK) {
      h2_atomic_bool_destroy(&self->stop_requested);
      return H2_PAL_ERR_NO_MEMORY;
    }
    h2_device_serial_config_t transport = config->transport;
    transport.stop_requested = &self->stop_requested;
    transport.sessions_admitted = &self->sessions_admitted;
    int rc = h2_device_serial_init_with_config(&self->transport, &transport);
    if (rc != H2_PAL_OK) {
      h2_atomic_bool_destroy(&self->sessions_admitted);
      h2_atomic_bool_destroy(&self->stop_requested);
      return rc;
    }
    self->io = h2_device_serial_io(&self->transport);
    self->client = client;
    self->task = config->task;
    self->initialized = 1;
  }
  self->transport.gate_started_ms = h2_device_serial_now(&self->transport);
  h2_atomic_bool_store(&self->stop_requested, false, H2_ATOMIC_RELEASE);
  const h2_loader_app_client_return_console_config_t console = {
      .client = client,
      .task = config->task,
      .read_user = self,
      .read_byte = app_read_byte,
      .write_user = self,
      .write = app_write,
      .task_name = config->task_name != NULL ? config->task_name
                                             : h2_loader_uart_command_task_name,
      .stack_size = config->stack_size,
  };
  self->started = 1;
  int rc = h2_loader_app_client_start_return_console(&console);
  if (rc != H2_PAL_OK) self->started = 0;
  return rc;
}
int h2_device_app_serial_start(h2_device_app_serial_t* self,
                               h2_loader_app_client_t* client,
                               const h2_pal_task_api_t* task,
                               const h2_pal_mem_api_t* allocator,
                               const h2_pal_time_api_t* time,
                               const h2_iostreamikcp_io_t* physical_io,
                               size_t stack_size) {
  if (physical_io == NULL) return H2_PAL_ERR_INVALID_ARG;
  const h2_device_app_serial_config_t config = {
      .transport = {.physical_io = *physical_io,
                    .allocator = allocator,
                    .time = time},
      .task = task,
      .stack_size = stack_size,
  };
  return h2_device_app_serial_start_with_config(self, client, &config);
}
void h2_device_app_serial_admit_sessions(h2_device_app_serial_t* self) {
  if (self == NULL) return;
  self->admission_requested = 1;
  if (self->initialized)
    h2_atomic_bool_store(&self->sessions_admitted, true, H2_ATOMIC_RELEASE);
}
int h2_device_app_serial_stop(h2_device_app_serial_t* self) {
  if (self == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!self->initialized) return H2_PAL_OK;
  h2_atomic_bool_store(&self->stop_requested, true, H2_ATOMIC_RELEASE);
  if (self->started) {
    int rc = h2_loader_app_client_stop_return_console(self->client);
    if (rc != H2_PAL_OK) return rc;
  }
  self->started = 0;
  h2_device_serial_deactivate(&self->transport);
  return H2_PAL_OK;
}
