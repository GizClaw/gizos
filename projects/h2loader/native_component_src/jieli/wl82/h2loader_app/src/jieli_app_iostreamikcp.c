#include "app_config.h"
#include "asm/includes.h"
#include "h2_command.h"
#include "h2_iostreamikcp.h"
#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_wl82_atomic.h"
#include "jieli_app_iostreamikcp.h"
#include "os/os_api.h"
#include "usb/device/cdc.h"
#include "usb/usb_common_def.h"

#include <stdint.h>
#include <string.h>

#ifndef EOF
#define EOF (-1)
#endif

enum {
  H2_USB_PACKET_SIZE = 64,
  H2_USB_RX_RING_SIZE = 8192,
  H2_PHYSICAL_READ_SIZE = 512,
  H2_PHYSICAL_POLL_MS = 10,
  H2_STREAM_RX_SIZE = 4096,
  H2_STREAM_RECEIVE_WINDOW = 20,
  H2_WRITE_TIMEOUT_MS = 5000,
  H2_MAX_FLUSH_TIMEOUT_MS = 120000,
  H2_SEGMENT_TIMEOUT_MS = 60,
  H2_MAX_WAITSND = 64,
  /* Match Loader's command task: status alone has an 11 KiB frame on WL82,
   * before nested metadata formatting and KCP I/O. PAL sizes are bytes. */
  H2_APP_COMMAND_STACK_SIZE = 48 * 1024,
  H2_USB_RX_TASK_STACK_SIZE = 4096,
  /* Upper bound on how long SESSION_OPEN is deferred while the App flushes its
   * boot and confirmation console on the raw pre-session path. The App normally
   * lifts the gate at confirmation (~1 s); this fallback keeps a boot that never
   * reaches confirmation from locking the host out of the command transport. */
  H2_SESSION_GATE_FALLBACK_MS = 8000,
};

typedef struct h2_jieli_app_transport {
  h2_iostreamikcp_io_t physical_io;
  h2_iostreamikcp_filter_t filter;
  h2_iostreamikcp_t *stream;
  const h2_pal_mem_api_t *allocator;
  uint32_t conv;
  uint32_t pending_conv;
  uint32_t write_timeout_ms;
  uint32_t io_started_ms;
  uint32_t io_timeout_ms;
  int io_deadline_active;
  int replacement_pending;
  int close_pending;
  /* Session admission gate. While closed, SESSION_OPEN is ignored so every
   * console byte already produced leaves on the raw pre-session path the host
   * captures losslessly; the App opens it once its confirmation has drained,
   * and a fallback deadline opens it regardless so a stuck boot cannot lock the
   * host out. Sticky once open. The launcher task writes it and the command
   * task reads it on the other core, so both sides use the wl82 atomics. */
  volatile uint32_t sessions_admitted;
  uint32_t gate_started_ms;
} h2_jieli_app_transport_t;

typedef struct h2_jieli_app_console {
  h2_jieli_app_transport_t transport;
  h2_command_io_api_t io;
  h2_loader_app_client_t *client;
  volatile usb_dev usb_id;
  OS_SEM rx_wakeup_sem;
  OS_SEM rx_data_sem;
  OS_MUTEX rx_mutex;
  OS_MUTEX tx_mutex;
  uint8_t rx_ring[H2_USB_RX_RING_SIZE];
  size_t rx_head;
  size_t rx_tail;
  size_t rx_count;
  uint32_t rx_overflow;
  int initialized;
  int started;
} h2_jieli_app_console_t;

static h2_jieli_app_console_t state;

int h2_jieli_usb_debug_try_lock(void) {
  if (!state.started || os_mutex_accept(&state.tx_mutex) != OS_NO_ERR) return 0;
  return 1;
}

void h2_jieli_usb_debug_unlock(void) {
  /* The SDK drain loop pairs unlock only with its own successful try-lock. */
  (void)os_mutex_post(&state.tx_mutex);
}

static uint32_t now_ms(void *user) {
  (void)user;
  return timer_get_ms();
}

static uint32_t ms_to_ticks(uint32_t ms) {
  uint32_t ticks = ms / 10u + (ms % 10u != 0u);
  return ticks == 0u ? 1u : ticks;
}

#if !defined CONFIG_H2_UART1_DEBUG_ENABLE
static void usb_cdc_wakeup(struct usb_device_t *device) {
  state.usb_id = usb_device2id(device);
  os_sem_post(&state.rx_wakeup_sem);
}

static void usb_rx_task(void *user) {
  h2_jieli_app_console_t *self = user;
  uint8_t packet[H2_USB_PACKET_SIZE];
  for (;;) {
    (void)os_sem_pend(&self->rx_wakeup_sem, 0u);
    for (uint32_t packets = 0u; packets < 32u; ++packets) {
      uint32_t received =
          cdc_read_data(self->usb_id, packet, sizeof(packet));
      if (received == 0u) break;
      if (os_mutex_pend(&self->rx_mutex, 0u) != OS_NO_ERR) continue;
      for (uint32_t i = 0u; i < received; ++i) {
        if (self->rx_count == sizeof(self->rx_ring)) {
          ++self->rx_overflow;
          break;
        }
        self->rx_ring[self->rx_head] = packet[i];
        self->rx_head = (self->rx_head + 1u) % sizeof(self->rx_ring);
        ++self->rx_count;
      }
      (void)os_mutex_post(&self->rx_mutex);
      os_sem_post(&self->rx_data_sem);
      if (packets == 31u) {
        os_sem_post(&self->rx_wakeup_sem);
        os_time_dly(1u);
      }
    }
  }
}

#endif

static int physical_read(
    void *user, void *buffer, size_t len, size_t *out_read,
    uint32_t timeout_ms) {
  h2_jieli_app_console_t *self = user;
  if (self == NULL || buffer == NULL || out_read == NULL || len == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_read = 0u;
  uint32_t started = timer_get_ms();
  for (;;) {
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
    int count = h2_jieli_ac791n_devkit_console_read(buffer, len);
    if (count < 0) return count;
    size_t take = (size_t)count;
#else
    if (os_mutex_pend(&self->rx_mutex, 0u) != OS_NO_ERR) {
      return H2_PAL_ERR_IO;
    }
    size_t take = len < self->rx_count ? len : self->rx_count;
    for (size_t i = 0u; i < take; ++i) {
      ((uint8_t *)buffer)[i] = self->rx_ring[self->rx_tail];
      self->rx_tail = (self->rx_tail + 1u) % sizeof(self->rx_ring);
    }
    self->rx_count -= take;
    (void)os_mutex_post(&self->rx_mutex);
#endif
    if (take != 0u) {
      *out_read = take;
      return H2_PAL_OK;
    }
    uint32_t elapsed = timer_get_ms() - started;
    if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    uint32_t remaining = timeout_ms - elapsed;
    uint32_t wait = remaining < H2_PHYSICAL_POLL_MS
                        ? remaining
                        : H2_PHYSICAL_POLL_MS;
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
    os_time_dly(ms_to_ticks(wait));
#else
    (void)os_sem_pend(&self->rx_data_sem, ms_to_ticks(wait));
#endif
  }
}

static int physical_write(
    void *user, const void *buffer, size_t len, size_t *out_written,
    uint32_t timeout_ms) {
  h2_jieli_app_console_t *self = user;
  const uint8_t *cursor = buffer;
  if (self == NULL || (buffer == NULL && len != 0u) || out_written == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_written = 0u;
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
  int count = h2_jieli_ac791n_devkit_console_write(buffer, len, timeout_ms);
  if (count < 0) return count;
  *out_written = (size_t)count;
  return *out_written == len ? H2_PAL_OK : H2_PAL_ERR_IO;
#else
  if (len == 0u) return H2_PAL_OK;
  uint32_t started = timer_get_ms();
  while (os_mutex_accept(&self->tx_mutex) != OS_NO_ERR) {
    if (timeout_ms == 0u) return H2_PAL_ERR_TIMEOUT;
    uint32_t elapsed = timer_get_ms() - started;
    if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    uint32_t ticks = (timeout_ms - elapsed) / 10u;
    if (ticks != 0u && os_mutex_pend(&self->tx_mutex, (int)ticks) == OS_NO_ERR) break;
  }
  int result = H2_PAL_OK;
  while (*out_written < len) {
    uint32_t elapsed = timeout_ms == 0u ? 0u : timer_get_ms() - started;
    if (timeout_ms != 0u && elapsed >= timeout_ms) {
      result = H2_PAL_ERR_TIMEOUT;
      break;
    }
    size_t remaining = len - *out_written;
    uint32_t take = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
    /* The board CDC patch performs one immediately available short packet;
     * it never waits on native TX or a trailing zero-length packet. */
    uint32_t written = cdc_write_data(
        self->usb_id, (uint8_t *)(uintptr_t)&cursor[*out_written], take);
    if (written > take) {
      result = H2_PAL_ERR_IO;
      break;
    }
    *out_written += written;
    if (written == 0u) {
      if (timeout_ms == 0u) {
        result = H2_PAL_ERR_TIMEOUT;
        break;
      }
      elapsed = timer_get_ms() - started;
      if (elapsed >= timeout_ms) {
        result = H2_PAL_ERR_TIMEOUT;
        break;
      }
      if (timeout_ms - elapsed >= 10u) os_time_dly(1u);
    }
  }
  (void)os_mutex_post(&self->tx_mutex);
  return result;
#endif
}

static int physical_flush(void *user) {
  (void)user;
  return H2_PAL_OK;
}

static void write_le32(uint8_t out[4], uint32_t value) {
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8u);
  out[2] = (uint8_t)(value >> 16u);
  out[3] = (uint8_t)(value >> 24u);
}

/* KCP may emit several frames in one call. Its per-frame default must not
 * restart the command caller's budget. The command task owns this scope. */
static h2_pal_result_t transport_read(
    void *user, void *buffer, size_t len, size_t *out_read,
    uint32_t timeout_ms) {
  h2_jieli_app_transport_t *self = user;
  return self->physical_io.read(
      self->physical_io.user, buffer, len, out_read, timeout_ms);
}

static h2_pal_result_t transport_write(
    void *user, const void *buffer, size_t len, size_t *out_written,
    uint32_t timeout_ms) {
  h2_jieli_app_transport_t *self = user;
  if (self->io_deadline_active) {
    uint32_t elapsed = timer_get_ms() - self->io_started_ms;
    if (self->io_timeout_ms != 0u && elapsed >= self->io_timeout_ms) {
      *out_written = 0u;
      return H2_PAL_ERR_TIMEOUT;
    }
    uint32_t remaining = elapsed >= self->io_timeout_ms
                             ? 0u : self->io_timeout_ms - elapsed;
    if (timeout_ms > remaining) timeout_ms = remaining;
  }
  return self->physical_io.write(
      self->physical_io.user, buffer, len, out_written, timeout_ms);
}

static h2_pal_result_t transport_flush(void *user) {
  h2_jieli_app_transport_t *self = user;
  return self->physical_io.flush == NULL ? H2_PAL_OK
      : self->physical_io.flush(self->physical_io.user);
}

static int send_control(
    h2_jieli_app_transport_t *self, uint8_t flags, uint32_t conv) {
  uint8_t payload[H2_IOSTREAMIKCP_SESSION_CONTROL_PAYLOAD_LEN];
  uint8_t encoded[H2_IOSTREAMIKCP_FRAME_HEADER_LEN +
                  H2_IOSTREAMIKCP_SESSION_CONTROL_PAYLOAD_LEN];
  size_t encoded_len = 0u;
  size_t written = 0u;
  write_le32(payload, conv);
  const h2_iostreamikcp_frame_t frame = {
      .flags = flags,
      .conv = conv,
      .payload = payload,
      .payload_len = sizeof(payload),
  };
  int rc = h2_iostreamikcp_frame_encode(
      &frame, encoded, sizeof(encoded), &encoded_len);
  if (rc == H2_PAL_OK) {
    rc = transport_write(
        self, encoded, encoded_len, &written,
        H2_WRITE_TIMEOUT_MS);
  }
  return rc == H2_PAL_OK && written == encoded_len
             ? H2_PAL_OK
             : (rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc);
}

static int on_frame(void *user, const h2_iostreamikcp_frame_t *frame) {
  h2_jieli_app_transport_t *self = user;
  if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN) {
    if (frame->conv == 0u) return H2_PAL_ERR_INVALID_ARG;
    /* Hold the session closed until the App has flushed its boot and
     * confirmation console. Answering here would move the console into the
     * reliable tunnel mid-flush, and the queued lines would be discarded when
     * the App proceeds; ignoring the frame makes the host retransmit it, and
     * the confirmation reaches the wire raw in the meantime. */
    if (h2_jieli_atomic_load_u32(&self->sessions_admitted) == 0u &&
        (uint32_t)(timer_get_ms() - self->gate_started_ms) <
            H2_SESSION_GATE_FALLBACK_MS) {
      return H2_PAL_OK;
    }
    h2_jieli_atomic_store_u32(&self->sessions_admitted, 1u);
    if (self->stream != NULL && frame->conv == self->conv) {
      return send_control(
          self, H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK, frame->conv);
    }
    self->pending_conv = frame->conv;
    self->replacement_pending = self->stream != NULL;
    return H2_PAL_OK;
  }
  if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_CLOSE &&
      self->stream != NULL && frame->conv == self->conv) {
    int rc = send_control(
        self, H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK, frame->conv);
    if (rc == H2_PAL_OK) {
      self->close_pending = 1;
      self->replacement_pending = 1;
    }
    return rc;
  }
  if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_DATA &&
      self->stream != NULL && frame->conv == self->conv) {
    return h2_iostreamikcp_input_frame(self->stream, frame);
  }
  return H2_PAL_OK;
}

static int poll_physical(
    h2_jieli_app_transport_t *self, uint32_t timeout_ms) {
  uint8_t buffer[H2_PHYSICAL_READ_SIZE];
  size_t count = 0u;
  uint32_t read_timeout = timeout_ms;
  if (self->stream != NULL && read_timeout > H2_PHYSICAL_POLL_MS) {
    read_timeout = H2_PHYSICAL_POLL_MS;
  }
  int rc = self->physical_io.read(
      self->physical_io.user, buffer, sizeof(buffer), &count, read_timeout);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
      rc != H2_PAL_ERR_WOULD_BLOCK) {
    return rc;
  }
  /* A callback can exhaust its deadline with complete frames still buffered.
   * Resume those frames even when the physical link supplies no new bytes. */
  int input_rc = h2_iostreamikcp_filter_input(
      &self->filter, buffer, count, on_frame, self);
  if (input_rc != H2_PAL_OK) return input_rc;
  if (self->stream != NULL) {
    int update_rc = h2_iostreamikcp_update(self->stream, timer_get_ms());
    if (update_rc != H2_PAL_OK) return update_rc;
  }
  return rc;
}

static void deactivate_current(h2_jieli_app_transport_t *self) {
  if (self == NULL) return;
  h2_iostreamikcp_close(self->stream);
  self->stream = NULL;
  self->conv = 0u;
  self->replacement_pending = 0;
  self->close_pending = 0;
}

static int activate_pending(h2_jieli_app_transport_t *self) {
  if (self->pending_conv == 0u) return H2_PAL_ERR_INVALID_STATE;
  uint32_t conv = self->pending_conv;
  deactivate_current(self);
  const h2_iostreamikcp_config_t config = {
      .io = {.user = self, .read = transport_read,
             .write = transport_write, .flush = transport_flush},
      .allocator = self->allocator,
      .now_ms = now_ms,
      .time_user = self,
      .conv = conv,
      .mtu = H2_IOSTREAMIKCP_DEFAULT_MTU,
      .rx_buffer_size = H2_STREAM_RX_SIZE,
      .receive_window = H2_STREAM_RECEIVE_WINDOW,
      .write_timeout_ms = H2_WRITE_TIMEOUT_MS,
  };
  int rc = h2_iostreamikcp_open(&config, &self->stream);
  if (rc != H2_PAL_OK) return rc;
  self->conv = conv;
  self->pending_conv = 0u;
  self->replacement_pending = 0;
  rc = send_control(self, H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK, conv);
  if (rc != H2_PAL_OK) {
    h2_iostreamikcp_close(self->stream);
    self->stream = NULL;
    self->conv = 0u;
  }
  return rc;
}

static int command_read_impl(
    void *user, void *buffer, size_t len, size_t *out_read,
    uint32_t timeout_ms) {
  h2_jieli_app_transport_t *self = user;
  if (self == NULL || buffer == NULL || out_read == NULL ||
      self->stream == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  uint32_t started = timer_get_ms();
  *out_read = 0u;
  for (;;) {
    if (self->replacement_pending) return H2_PAL_ERR_CLOSED;
    int rc = h2_iostreamikcp_read(self->stream, buffer, len, out_read);
    if (rc == H2_PAL_OK && *out_read != 0u) return H2_PAL_OK;
    if (rc != H2_PAL_ERR_WOULD_BLOCK) return rc;
    uint32_t elapsed = timer_get_ms() - started;
    if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    rc = poll_physical(self, timeout_ms - elapsed);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
        rc != H2_PAL_ERR_WOULD_BLOCK) {
      return rc;
    }
  }
}

static int command_read(
    void *user, void *buffer, size_t len, size_t *out_read,
    uint32_t timeout_ms) {
  h2_jieli_app_transport_t *self = user;
  if (self == NULL) return H2_PAL_ERR_INVALID_ARG;
  self->io_started_ms = timer_get_ms();
  self->io_timeout_ms = timeout_ms;
  self->io_deadline_active = 1;
  int result = command_read_impl(user, buffer, len, out_read, timeout_ms);
  self->io_deadline_active = 0;
  return result;
}

static int command_write_impl(
    void *user, const void *buffer, size_t len, size_t *out_written,
    uint32_t timeout_ms) {
  h2_jieli_app_transport_t *self = user;
  if (self == NULL || (buffer == NULL && len != 0u) ||
      out_written == NULL || self->stream == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (self->replacement_pending) return H2_PAL_ERR_CLOSED;
  self->write_timeout_ms = timeout_ms;
  *out_written = 0u;
  uint32_t started = timer_get_ms();
  for (;;) {
    h2_iostreamikcp_stats_t stats;
    int rc = h2_iostreamikcp_get_stats(self->stream, &stats);
    if (rc != H2_PAL_OK) return rc;
    if (stats.waitsnd < H2_MAX_WAITSND) break;
    if (self->replacement_pending) return H2_PAL_ERR_CLOSED;
    if (timer_get_ms() - started >= self->write_timeout_ms) {
      return H2_PAL_ERR_TIMEOUT;
    }
    uint32_t elapsed = timer_get_ms() - started;
    uint32_t remaining = self->write_timeout_ms - elapsed;
    uint32_t poll_ms = remaining < H2_PHYSICAL_POLL_MS
                           ? remaining
                           : H2_PHYSICAL_POLL_MS;
    rc = poll_physical(self, poll_ms);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
        rc != H2_PAL_ERR_WOULD_BLOCK) {
      return rc;
    }
  }
  int rc = h2_iostreamikcp_write(self->stream, buffer, len);
  if (rc == H2_PAL_OK) *out_written = len;
  if (rc == H2_PAL_OK) {
    int poll_rc = poll_physical(self, 0u);
    if (poll_rc != H2_PAL_OK && poll_rc != H2_PAL_ERR_TIMEOUT &&
        poll_rc != H2_PAL_ERR_WOULD_BLOCK) {
      return poll_rc;
    }
  }
  return rc;
}

static int command_write(
    void *user, const void *buffer, size_t len, size_t *out_written,
    uint32_t timeout_ms) {
  h2_jieli_app_transport_t *self = user;
  if (self == NULL) return H2_PAL_ERR_INVALID_ARG;
  self->io_started_ms = timer_get_ms();
  self->io_timeout_ms = timeout_ms;
  self->io_deadline_active = 1;
  int result = command_write_impl(user, buffer, len, out_written, timeout_ms);
  self->io_deadline_active = 0;
  return result;
}

static int command_flush_impl(void *user) {
  h2_jieli_app_transport_t *self = user;
  if (self == NULL || self->stream == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (self->replacement_pending) return H2_PAL_ERR_CLOSED;
  h2_iostreamikcp_stats_t stats = {0};
  int rc = h2_iostreamikcp_flush(self->stream);
  if (rc == H2_PAL_OK) rc = h2_iostreamikcp_get_stats(self->stream, &stats);
  uint64_t estimate = (uint64_t)stats.waitsnd * H2_SEGMENT_TIMEOUT_MS;
  uint32_t timeout_ms = self->write_timeout_ms;
  if (estimate > timeout_ms) {
    timeout_ms = estimate > H2_MAX_FLUSH_TIMEOUT_MS
                     ? H2_MAX_FLUSH_TIMEOUT_MS
                     : (uint32_t)estimate;
  }
  uint32_t started = timer_get_ms();
  while (rc == H2_PAL_OK) {
    rc = h2_iostreamikcp_get_stats(self->stream, &stats);
    if (rc != H2_PAL_OK || stats.waitsnd == 0u) break;
    if (self->replacement_pending) return H2_PAL_ERR_CLOSED;
    uint32_t elapsed = timer_get_ms() - started;
    if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    uint32_t remaining = timeout_ms - elapsed;
    uint32_t poll_ms = remaining < H2_PHYSICAL_POLL_MS
                           ? remaining
                           : H2_PHYSICAL_POLL_MS;
    rc = poll_physical(self, poll_ms);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
      rc = h2_iostreamikcp_update(self->stream, timer_get_ms());
    }
  }
  return rc;
}

static int command_flush(void *user) {
  h2_jieli_app_transport_t *self = user;
  if (self == NULL || self->stream == NULL) return H2_PAL_ERR_INVALID_STATE;
  h2_iostreamikcp_stats_t stats = {0};
  int result = h2_iostreamikcp_get_stats(self->stream, &stats);
  if (result != H2_PAL_OK) return result;
  uint64_t estimate = (uint64_t)stats.waitsnd * H2_SEGMENT_TIMEOUT_MS;
  uint32_t budget = self->write_timeout_ms;
  if (estimate > budget) {
    budget = estimate > H2_MAX_FLUSH_TIMEOUT_MS
                 ? H2_MAX_FLUSH_TIMEOUT_MS : (uint32_t)estimate;
  }
  self->io_started_ms = timer_get_ms();
  self->io_timeout_ms = budget;
  self->io_deadline_active = 1;
  result = command_flush_impl(user);
  self->io_deadline_active = 0;
  return result;
}

static const h2_command_io_vtable_t command_io_vtable = {
    .read = command_read,
    .write = command_write,
    .flush = command_flush,
};

static int app_read_byte(void *user, uint32_t timeout_ms) {
  h2_jieli_app_console_t *self = user;
  uint8_t value = 0u;
  size_t count = 0u;
  if (self == NULL) return EOF;
  for (;;) {
    if (self->transport.close_pending &&
        self->transport.pending_conv == 0u) {
      deactivate_current(&self->transport);
      return EOF;
    }
    if (self->transport.pending_conv != 0u) {
      int rc = activate_pending(&self->transport);
      return rc == H2_PAL_OK ? H2_LOADER_APP_CLIENT_SESSION_RESET : EOF;
    }
    if (self->transport.stream == NULL) {
      int rc = poll_physical(&self->transport, timeout_ms);
      if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
          rc != H2_PAL_ERR_WOULD_BLOCK) {
        return EOF;
      }
      if (self->transport.pending_conv == 0u) return EOF;
      continue;
    }
    int rc = self->io.vtable->read(
        self->io.user, &value, sizeof(value), &count, timeout_ms);
    if (rc == H2_PAL_OK && count == sizeof(value)) return value;
    if (rc == H2_PAL_ERR_CLOSED && self->transport.replacement_pending) {
      continue;
    }
    return EOF;
  }
}

static int app_write(void *user, const char *data, size_t len) {
  h2_jieli_app_console_t *self = user;
  size_t offset = 0u;
  if (self == NULL || (data == NULL && len != 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  while (offset < len) {
    size_t written = 0u;
    int rc = self->io.vtable->write(
        self->io.user, data + offset, len - offset, &written,
        H2_WRITE_TIMEOUT_MS);
    if (rc != H2_PAL_OK) return rc;
    if (written == 0u || written > len - offset) return H2_PAL_ERR_IO;
    offset += written;
  }
  return self->io.vtable->flush(self->io.user);
}

int h2_jieli_app_iostreamikcp_log(const char *data, size_t len) {
  if (!state.started || (data == NULL && len != 0u)) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  size_t written = 0u;
  return physical_write(&state, data, len, &written, H2_WRITE_TIMEOUT_MS) ==
                 H2_PAL_OK &&
             written == len
         ? H2_PAL_OK
         : H2_PAL_ERR_IO;
}

/* No callbacks or tasks may observe these objects until every create succeeds. */
static int console_sync_init(h2_jieli_app_console_t *self) {
  if (os_mutex_create(&self->rx_mutex) != OS_NO_ERR) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  if (os_mutex_create(&self->tx_mutex) != OS_NO_ERR) {
    goto fail_tx;
  }
  if (os_sem_create(&self->rx_wakeup_sem, 0) != OS_NO_ERR) {
    goto fail_wakeup;
  }
  if (os_sem_create(&self->rx_data_sem, 0) != OS_NO_ERR) {
    goto fail_data;
  }
  return H2_PAL_OK;

fail_data:
  (void)os_sem_del(&self->rx_wakeup_sem, 0);
fail_wakeup:
  (void)os_mutex_del(&self->tx_mutex, 0);
fail_tx:
  (void)os_mutex_del(&self->rx_mutex, 0);
  return H2_PAL_ERR_NO_MEMORY;
}

/* Only valid before callbacks/workers have been published. */
static void console_sync_destroy(h2_jieli_app_console_t *self) {
  (void)os_sem_del(&self->rx_data_sem, 0);
  (void)os_sem_del(&self->rx_wakeup_sem, 0);
  (void)os_mutex_del(&self->tx_mutex, 0);
  (void)os_mutex_del(&self->rx_mutex, 0);
}

int h2_jieli_app_iostreamikcp_start(
    h2_loader_app_client_t *client,
    const h2_pal_task_api_t *task,
    const h2_pal_mem_api_t *allocator) {
  if (client == NULL || task == NULL || allocator == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (state.started) return H2_PAL_ERR_INVALID_STATE;
  /* A failed command-task start does not stop the physical console. Reuse its
   * live objects on retry; never overwrite storage reachable by the RX task. */
  if (state.initialized) {
    if (state.client != client || state.transport.allocator != allocator) {
      return H2_PAL_ERR_INVALID_STATE;
    }
    goto start_command;
  }
  /* A session admission requested before the first start is sticky: carry
   * it across the fresh initialization so it opens the gate for this boot. */
  const uint32_t admitted =
      h2_jieli_atomic_load_u32(&state.transport.sessions_admitted);
  memset(&state, 0, sizeof(state));
  h2_jieli_atomic_store_u32(&state.transport.sessions_admitted, admitted);
  state.client = client;
  state.transport.allocator = allocator;
  state.transport.physical_io = (h2_iostreamikcp_io_t){
      .user = &state,
      .read = physical_read,
      .write = physical_write,
      .flush = physical_flush,
  };
  h2_iostreamikcp_filter_init(&state.transport.filter);
  state.io = (h2_command_io_api_t){
      .user = &state.transport,
      .vtable = &command_io_vtable,
  };
  int sync_result = console_sync_init(&state);
  if (sync_result != H2_PAL_OK) return sync_result;
  state.usb_id = 0;
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
  if (h2_jieli_ac791n_devkit_console_start() != H2_PAL_OK) {
    console_sync_destroy(&state);
    return H2_PAL_ERR_IO;
  }
#else
  if (h2_jieli_ac791n_devkit_usb_debug_start() != H2_PAL_OK) {
    console_sync_destroy(&state);
    return H2_PAL_ERR_IO;
  }
  if (os_task_create(
          usb_rx_task, &state, 12, H2_USB_RX_TASK_STACK_SIZE, 0,
          "h2app_rx") != OS_NO_ERR) {
    console_sync_destroy(&state);
    return H2_PAL_ERR_NO_MEMORY;
  }
  for (usb_dev id = 0; id < USB_MAX_HW_NUM; ++id) {
    cdc_set_wakeup_handler(id, usb_cdc_wakeup);
  }
#endif
  state.initialized = 1;
start_command:
  /* Arm the fallback deadline now: the command task begins polling as soon as
   * it is created, and the host may send SESSION_OPEN before the App confirms.
   * The gate itself starts closed on a fresh boot and is never reset here, so
   * an admission requested before start (or before a command-task retry)
   * stays in force. */
  state.transport.gate_started_ms = timer_get_ms();
  state.started = 1;
  const h2_loader_app_client_return_console_config_t console = {
      .client = client,
      .task = task,
      .read_user = &state,
      .read_byte = app_read_byte,
      .write_user = &state,
      .write = app_write,
      .task_name = "h2loader/appcmd",
      .stack_size = H2_APP_COMMAND_STACK_SIZE,
  };
  int rc = h2_loader_app_client_start_return_console(&console);
  if (rc != H2_PAL_OK) state.started = 0;
  return rc;
}

void h2_jieli_app_iostreamikcp_admit_sessions(void) {
  /* Idempotent, sticky, and safe before start: start only arms the fallback
   * deadline and never clears the flag, so a pre-start admission opens the
   * gate for the transport that starts later. Published atomically for the
   * command task on the other core. */
  h2_jieli_atomic_store_u32(&state.transport.sessions_admitted, 1u);
}
