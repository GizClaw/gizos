#include "app_config.h"
#include "asm/includes.h"
#include "asm/system_reset_reason.h"
#include "h2_iostreamikcp.h"
#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_partitions.h"
#include "h2_loader_boot.h"
#include "h2_loader_command.h"
#include "h2loader_app.h"
#include "h2loader_bleikcp_internal.h"
#include "h2_loader_sha256.h"
#include "jieli_loader_platform.h"
#include "os/os_api.h"
#include "system/task.h"
#include "update/dual_bank_updata_api.h"
#include "usb/device/cdc.h"
#include "usb/usb_common_def.h"

#include <string.h>

extern int printf(const char *format, ...);
extern int puts(const char *text);
extern int snprintf(char *buffer, size_t size, const char *format, ...);

enum {
  H2_USB_PACKET_SIZE = 64,
  H2_PHYSICAL_READ_SIZE = 512,
  H2_PHYSICAL_POLL_MS = 10,
  H2_STREAM_RX_SIZE = 4096,
  H2_STREAM_RECEIVE_WINDOW = 20,
  H2_WRITE_TIMEOUT_MS = 5000,
  H2_MAX_FLUSH_TIMEOUT_MS = 120000,
  H2_SEGMENT_TIMEOUT_MS = 60,
  H2_MAX_WAITSND = 64,
  H2_SESSION_IDLE_TIMEOUT_MS = 30000,
  H2_LOADER_TASK_STACK_SIZE = 16384,
  H2_USB_RX_TASK_STACK_SIZE = 4096,
};

typedef struct h2_jieli_transport {
  h2_iostreamikcp_io_t physical_io;
  h2_iostreamikcp_filter_t filter;
  h2_iostreamikcp_t *stream;
  const h2_pal_mem_api_t *allocator;
  uint32_t conv;
  uint32_t pending_conv;
  uint32_t write_timeout_ms;
  uint32_t last_frame_ms;
  int replacement_pending;
  int close_pending;
} h2_jieli_transport_t;

typedef struct h2_jieli_digest {
  h2_loader_sha256_t sha;
  uint32_t bytes;
  int active;
} h2_jieli_digest_t;

static OS_MUTEX usb_tx_mutex;
static volatile int usb_tx_ready;
static volatile int usb_debug_locked;
static volatile int crash_recovery_active;
static h2_jieli_transport_t transport;
static h2_jieli_digest_t digest;
static volatile int stop_requested;
/* Single-writer RX counters; the USB log task only samples them. A snapshot
 * can span two updates, but never dereferences the mutable KCP session. */
static volatile uint32_t cdc_rx_phase;
static volatile uint32_t cdc_rx_calls;
static volatile uint32_t cdc_rx_busy;
static volatile uint32_t cdc_rx_bytes;
static volatile uint32_t link_frames;
static volatile uint32_t link_last_flags;
static volatile uint32_t link_last_frame_ms;

void h2_jieli_usb_cdc_rx_trace(uint32_t phase, uint32_t bytes) {
  cdc_rx_phase = phase;
  if (phase == 1u) ++cdc_rx_calls;
  if (phase == 2u) ++cdc_rx_busy;
  if (phase == 4u) cdc_rx_bytes += bytes;
}

extern int h2_jieli_wl82_take_crash_pending(void);
extern int h2_jieli_wl82_coredump_flush_pending(void);

static uint32_t ms_to_ticks(uint32_t ms) {
  uint32_t ticks = (ms + 9u) / 10u;
  return ticks == 0u ? 1u : ticks;
}

/* The SDK USB printf task uses these hooks (through the target-local SDK
 * patch) so an entire drained printf batch cannot split an iKCP frame. */
int h2_jieli_usb_debug_try_lock(void) {
  usb_debug_locked = 0;
  if (!usb_tx_ready || os_mutex_accept(&usb_tx_mutex) != OS_NO_ERR) return 0;
  usb_debug_locked = 1;
  return 1;
}

void h2_jieli_usb_debug_unlock(void) {
  if (usb_debug_locked) {
    usb_debug_locked = 0;
    (void)os_mutex_post(&usb_tx_mutex);
  }
}

int h2_jieli_usb_debug_is_draining(void) {
  return usb_debug_locked;
}

static void usb_diag_write(const char *text) {
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
  if (text != NULL) {
    (void)h2_jieli_ac791n_devkit_console_write(text, strlen(text), 100);
  }
#else
  if (text == NULL || !usb_tx_ready) return;
  if (os_mutex_accept(&usb_tx_mutex) != OS_NO_ERR) return;
  size_t len = strlen(text);
  while (len != 0u) {
    uint32_t take = len > UINT32_MAX ? UINT32_MAX : (uint32_t)len;
    uint32_t written =
        cdc_write_data(0, (uint8_t *)(uintptr_t)text, take);
    /* Diagnostics are best-effort: before the host opens CDC there is no TX
     * consumer, and blocking here would prevent Loader startup entirely. */
    if (written == 0u) break;
    text += written;
    len -= (size_t)written;
  }
  (void)os_mutex_post(&usb_tx_mutex);
#endif
}

void h2_jieli_sd_fs_trace_write(
    const char *stage, size_t offset, size_t length, int result) {
  char line[160];
  (void)snprintf(
      line, sizeof(line),
      "H2_JIELI_SD_WRITE phase=%s offset=%u length=%u result=%d\r\n",
      stage != NULL ? stage : "unknown", (unsigned)offset,
      (unsigned)length, result);
  usb_diag_write(line);
}

void h2_jieli_loader_diag_write(const char *text) {
  usb_diag_write(text);
}

void h2_jieli_usb_debug_heartbeat(void) {
#ifdef H2_JIELI_BLE_DIAG_QUIET_UART
  return;
#else
  char line[224];
  (void)snprintf(
      line, sizeof(line),
      "H2_JIELI_LINK ms=%u rx_phase=%u rx_calls=%u rx_busy=%u rx_bytes=%u "
      "frames=%u last_flags=%u last_frame_ms=%u fs=%s\r\n",
      (unsigned)timer_get_ms(), (unsigned)cdc_rx_phase,
      (unsigned)cdc_rx_calls, (unsigned)cdc_rx_busy, (unsigned)cdc_rx_bytes,
      (unsigned)link_frames, (unsigned)link_last_flags,
      (unsigned)link_last_frame_ms,
      h2_jieli_ac791n_devkit_sd_fs_last_stage());
  usb_diag_write(line);
#endif
}

static void usb_diag_error_forever(const char *step, int code) {
  char line[96];
  (void)snprintf(
      line, sizeof(line),
      "H2_JIELI_LOADER_ERROR step=%s code=%d\r\n", step, code);
  for (;;) {
    usb_diag_write(line);
    os_time_dly(100u);
  }
}

static uint32_t now_ms32(void *user) {
  (void)user;
  return timer_get_ms();
}

static uint64_t now_ms64(void *user) {
  (void)user;
  return timer_get_ms();
}

static void sleep_ms(void *user, uint32_t delay_ms) {
  (void)user;
  os_time_dly((delay_ms + 9u) / 10u);
}

static int physical_read(
    void *user, void *buffer, size_t len, size_t *out_read,
    uint32_t timeout_ms) {
  (void)user;
  if (buffer == NULL || out_read == NULL || len == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_read = 0u;
  const uint32_t started = timer_get_ms();
  for (;;) {
    uint32_t requested = len > UINT32_MAX ? UINT32_MAX : (uint32_t)len;
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
#if defined H2_JIELI_BLE_DIAG_PAUSE_UART_READ
    /* Diagnostic only: the official BLE example uses its UART for TX logging
     * but does not continuously call dev_read().  Suppress those reads for a
     * bounded boot window while leaving UART TX alive.  RX automatically
     * resumes, so a failed BLE experiment cannot strand the Loader. */
    const uint32_t now = timer_get_ms();
    if (now >= 5000u && now < 90000u) {
      h2_jieli_usb_cdc_rx_trace(5, 0);
      os_time_dly(ms_to_ticks(H2_PHYSICAL_POLL_MS));
      continue;
    }
#endif
    h2_jieli_usb_cdc_rx_trace(1, 0);
    int count = h2_jieli_ac791n_devkit_console_read(buffer, requested);
    if (count < 0) return count;
    uint32_t received = (uint32_t)count;
    h2_jieli_usb_cdc_rx_trace(4, received);
#else
    uint32_t received = cdc_read_data(0, (uint8_t *)buffer, requested);
#endif
    if (received != 0u) {
      *out_read = received;
      return H2_PAL_OK;
    }
    uint32_t elapsed = timer_get_ms() - started;
    if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    os_time_dly(ms_to_ticks(H2_PHYSICAL_POLL_MS));
  }
}

static int physical_write(
    void *user, const void *buffer, size_t len, size_t *out_written,
    uint32_t timeout_ms) {
  const uint8_t *cursor = buffer;
  (void)user;
  if ((buffer == NULL && len != 0u) || out_written == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_written = 0u;
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
  int count = h2_jieli_ac791n_devkit_console_write(buffer, len, timeout_ms);
  if (count < 0) return count;
  *out_written = (size_t)count;
  return H2_PAL_OK;
#else
  if (os_mutex_pend(&usb_tx_mutex, (int)ms_to_ticks(timeout_ms)) !=
      OS_NO_ERR) {
    return H2_PAL_ERR_IO;
  }
  uint32_t started = timer_get_ms();
  while (*out_written < len) {
    size_t remaining = len - *out_written;
    uint32_t take = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
    uint32_t written = cdc_write_data(
        0, (uint8_t *)(uintptr_t)&cursor[*out_written], take);
    if (written != 0u) {
      *out_written += (size_t)written;
    } else if (timer_get_ms() - started >= timeout_ms) {
      (void)os_mutex_post(&usb_tx_mutex);
      return H2_PAL_ERR_TIMEOUT;
    } else {
      os_time_dly(1u);
    }
  }
  (void)os_mutex_post(&usb_tx_mutex);
  return H2_PAL_OK;
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

static int send_control(
    h2_jieli_transport_t *self, uint8_t flags, uint32_t conv) {
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
    rc = self->physical_io.write(
        self->physical_io.user, encoded, encoded_len, &written,
        H2_WRITE_TIMEOUT_MS);
  }
  return rc == H2_PAL_OK && written == encoded_len
             ? H2_PAL_OK
             : (rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc);
}

static int on_frame(void *user, const h2_iostreamikcp_frame_t *frame) {
  h2_jieli_transport_t *self = user;
  ++link_frames;
  link_last_flags = frame->flags;
  link_last_frame_ms = timer_get_ms();
  self->last_frame_ms = timer_get_ms();
  if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN) {
    if (frame->conv == 0u) return H2_PAL_ERR_INVALID_ARG;
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
    h2_jieli_transport_t *self, uint32_t timeout_ms) {
  uint8_t buffer[H2_PHYSICAL_READ_SIZE];
  size_t count = 0u;
  uint32_t read_timeout_ms = timeout_ms;
  if (stop_requested) return H2_PAL_ERR_CLOSED;
  if (self->stream != NULL && read_timeout_ms > H2_PHYSICAL_POLL_MS) {
    read_timeout_ms = H2_PHYSICAL_POLL_MS;
  }
  int rc = self->physical_io.read(
      self->physical_io.user, buffer, sizeof(buffer), &count, read_timeout_ms);
  if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
      rc != H2_PAL_ERR_WOULD_BLOCK) {
    return rc;
  }
  if (count != 0u) {
    int input_rc = h2_iostreamikcp_filter_input(
        &self->filter, buffer, count, on_frame, self);
    if (input_rc != H2_PAL_OK) return input_rc;
  }
  if (self->stream != NULL) {
    int update_rc = h2_iostreamikcp_update(self->stream, timer_get_ms());
    if (update_rc != H2_PAL_OK) return update_rc;
  }
  return rc;
}

static void deactivate_current(h2_jieli_transport_t *self) {
  if (self == NULL) return;
  h2_iostreamikcp_close(self->stream);
  self->stream = NULL;
  self->conv = 0u;
  self->last_frame_ms = 0u;
  self->replacement_pending = 0;
  self->close_pending = 0;
}

static int activate_pending(h2_jieli_transport_t *self) {
  if (self->pending_conv == 0u) return H2_PAL_ERR_INVALID_STATE;
  uint32_t conv = self->pending_conv;
  deactivate_current(self);
  const h2_iostreamikcp_config_t config = {
      .io = self->physical_io,
      .allocator = self->allocator,
      .now_ms = now_ms32,
      .time_user = self,
      .conv = conv,
      .mtu = H2_IOSTREAMIKCP_DEFAULT_MTU,
      .rx_buffer_size = H2_STREAM_RX_SIZE,
      .receive_window = H2_STREAM_RECEIVE_WINDOW,
      .write_timeout_ms = H2_WRITE_TIMEOUT_MS,
  };
  int rc = h2_iostreamikcp_open(&config, &self->stream);
  if (rc != H2_PAL_OK) {
    char line[96];
    (void)snprintf(
        line, sizeof(line),
        "H2_JIELI_LOADER_ERROR step=session_open code=%d\r\n", rc);
    usb_diag_write(line);
    self->pending_conv = 0u;
    self->replacement_pending = 0;
    self->conv = 0u;
    return rc;
  }
  self->conv = conv;
  self->last_frame_ms = timer_get_ms();
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

static int command_read(
    void *user, void *buffer, size_t len, size_t *out_read,
    uint32_t timeout_ms) {
  h2_jieli_transport_t *self = user;
  if (self == NULL || buffer == NULL || out_read == NULL ||
      self->stream == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  uint32_t started = timer_get_ms();
  *out_read = 0u;
  for (;;) {
    if (stop_requested || self->replacement_pending) {
      return H2_PAL_ERR_CLOSED;
    }
    int rc = h2_iostreamikcp_read(
        self->stream, buffer, len, out_read);
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

static int command_write(
    void *user, const void *buffer, size_t len, size_t *out_written,
    uint32_t timeout_ms) {
  h2_jieli_transport_t *self = user;
  if (self == NULL || (buffer == NULL && len != 0u) ||
      out_written == NULL || self->stream == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (stop_requested || self->replacement_pending) {
    return H2_PAL_ERR_CLOSED;
  }
  self->write_timeout_ms = timeout_ms == 0u
                               ? H2_WRITE_TIMEOUT_MS
                               : timeout_ms;
  *out_written = 0u;
  uint32_t started = timer_get_ms();
  for (;;) {
    h2_iostreamikcp_stats_t stats;
    int rc = h2_iostreamikcp_get_stats(self->stream, &stats);
    if (rc != H2_PAL_OK) return rc;
    if (stats.waitsnd < H2_MAX_WAITSND) break;
    if (stop_requested || self->replacement_pending) {
      return H2_PAL_ERR_CLOSED;
    }
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

static int command_flush(void *user) {
  h2_jieli_transport_t *self = user;
  if (self == NULL || self->stream == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (stop_requested || self->replacement_pending) {
    return H2_PAL_ERR_CLOSED;
  }
  h2_iostreamikcp_stats_t stats = {0};
  int rc = h2_iostreamikcp_flush(self->stream);
  if (rc == H2_PAL_OK) {
    rc = h2_iostreamikcp_get_stats(self->stream, &stats);
  }
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
    if (stop_requested || self->replacement_pending) {
      return H2_PAL_ERR_CLOSED;
    }
    uint32_t elapsed = timer_get_ms() - started;
    if (elapsed >= timeout_ms) {
      return H2_PAL_ERR_TIMEOUT;
    }
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

static const h2_command_io_vtable_t command_io_vtable = {
    .read = command_read,
    .write = command_write,
    .flush = command_flush,
};

static int digest_start(void *user) {
  h2_jieli_digest_t *self = user;
  h2_loader_sha256_init(&self->sha);
  self->bytes = 0u;
  self->active = 1;
  return H2_PAL_OK;
}

static int digest_update(
    void *user, const uint8_t *data, size_t len) {
  h2_jieli_digest_t *self = user;
  if (!self->active || (data == NULL && len != 0u)) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_loader_sha256_update(&self->sha, data, len);
  self->bytes += (uint32_t)len;
  return H2_PAL_OK;
}

static int digest_finish(void *user, uint8_t out[32]) {
  h2_jieli_digest_t *self = user;
  char digest_hex[65];
  char line[128];
  if (!self->active || out == NULL) return H2_PAL_ERR_INVALID_STATE;
  h2_loader_sha256_finish(&self->sha, out);
  h2_loader_sha256_hex(out, digest_hex);
  (void)snprintf(
      line, sizeof(line),
      "H2_JIELI_DIGEST_FINISH bytes=%u sha256=%s\r\n",
      self->bytes, digest_hex);
  usb_diag_write(line);
  self->active = 0;
  return H2_PAL_OK;
}

static void digest_abort(void *user) {
  h2_jieli_digest_t *self = user;
  if (self->active) {
    char line[64];
    (void)snprintf(
        line, sizeof(line),
        "H2_JIELI_DIGEST_ABORT bytes=%u\r\n", self->bytes);
    usb_diag_write(line);
  }
  memset(self, 0, sizeof(*self));
}

static int clear_data(void *user, const char *path) {
  return h2_pal_fs_clear(user, path);
}

static int current_loader_identity(
    const h2_runtime_t *runtime, const char *version,
    h2_loader_image_identity_t *out_identity) {
  h2_pal_power_boot_partition_t running;
  h2_loader_status_t status;
  const h2_loader_metadata_t *persisted = NULL;
  struct BootInfo boot_info;
  h2_loader_sha256_t sha;
  uint8_t digest_value[32];
  int rc;

  if (runtime == NULL || version == NULL || out_identity == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  rc = h2_pal_power_get_running_boot_partition(runtime->power, &running);
  if (rc != H2_PAL_OK) return rc;
  memset(&status, 0, sizeof(status));
  rc = h2_loader_read_pref_status(runtime->pref, runtime->mem, &status);
  if (rc == H2_PAL_OK) {
    persisted = running.id == H2_JIELI_PARTITION_LOADER
                    ? &status.partition_1
                    : &status.partition_2;
  }

  memset(out_identity, 0, sizeof(*out_identity));
  out_identity->format = 1u;
  out_identity->role = H2_LOADER_IMAGE_ROLE_H2LOADER;
  (void)snprintf(
      out_identity->board, sizeof(out_identity->board), "%s",
      runtime->board);
  (void)snprintf(
      out_identity->target, sizeof(out_identity->target), "%s",
      runtime->target);
  (void)snprintf(
      out_identity->version, sizeof(out_identity->version), "%s", version);
  if (persisted != NULL && persisted->valid &&
      persisted->role == H2_LOADER_IMAGE_ROLE_H2LOADER &&
      persisted->image_size != 0u &&
      strcmp(persisted->board, runtime->board) == 0 &&
      strcmp(persisted->target, runtime->target) == 0) {
    /* The verified package owns its version. It may intentionally differ
     * from the binary's build label. Falling back to a BootInfo-derived
     * identity here loses the UFW size/hash needed by self-upgrade copy. */
    (void)snprintf(out_identity->version, sizeof(out_identity->version),
                   "%s", persisted->version);
    out_identity->image_size = persisted->image_size;
    (void)snprintf(
        out_identity->image_sha256, sizeof(out_identity->image_sha256),
        "%s", persisted->image_checksum);
    usb_diag_write("H2_JIELI_ACTIVE_IDENTITY source=pref\r\n");
    return H2_PAL_OK;
  }

  memset(&boot_info, 0, sizeof(boot_info));
  if (get_current_boot_info(&boot_info) != 0 || boot_info.codeLength == 0u) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  /* On a factory/full-flash boot there is no persisted package identity yet.
   * Do not read the currently executing bank through reserve-zone APIs: the
   * WL82 flash controller can block that access indefinitely.  Seed a stable
   * bootstrap identity from authenticated boot metadata plus the build and
   * target identity.  A Loader-installed image always takes the persisted,
   * package-verified identity path above. */
  h2_loader_sha256_init(&sha);
  h2_loader_sha256_update(
      &sha, (const uint8_t *)&boot_info, sizeof(boot_info));
  h2_loader_sha256_update(
      &sha, (const uint8_t *)version, strlen(version));
  h2_loader_sha256_update(
      &sha, (const uint8_t *)runtime->board, strlen(runtime->board));
  h2_loader_sha256_update(
      &sha, (const uint8_t *)runtime->target, strlen(runtime->target));
  h2_loader_sha256_finish(&sha, digest_value);
  h2_loader_sha256_hex(digest_value, out_identity->image_sha256);
  out_identity->image_size = boot_info.codeLength;
  usb_diag_write("H2_JIELI_ACTIVE_IDENTITY source=flash\r\n");
  return H2_PAL_OK;
}

static void loader_startup_event(
    void *user, h2_loader_startup_event_t event, int code) {
  char line[96];
  (void)user;
  (void)snprintf(
      line, sizeof(line),
      "H2_JIELI_STARTUP_EVENT event=%u code=%d\r\n",
      (unsigned)event, code);
  usb_diag_write(line);
}

static int prepare_loader(void *user, h2_loader_t *loader) {
  h2_runtime_t *runtime = user;
  (void)loader;
  int rc = h2_loader_package_recover_publish(
      runtime->fs, runtime->pref, runtime->mem,
      H2_LOADER_DEFAULT_PACKAGE_PATH,
      "/dl/update.tar.zlib.prev");
  if (rc != H2_PAL_OK) return rc;
  /* The portable Loader interprets a prepare failure as force-command-mode.
   * Preserve that public behavior so recovery cannot jump into the App. */
  return crash_recovery_active ? H2_PAL_ERR_INVALID_STATE : H2_PAL_OK;
}

static int serve_loader(
    void *user, h2_loader_t *loader, h2_loader_command_t *command,
    h2_loader_startup_action_t action) {
  h2_runtime_t *runtime = user;
  (void)loader;
  (void)action;
  memset(&transport, 0, sizeof(transport));
  transport.allocator = runtime->mem;
  transport.write_timeout_ms = H2_WRITE_TIMEOUT_MS;
  transport.physical_io = (h2_iostreamikcp_io_t){
      .user = &transport,
      .read = physical_read,
      .write = physical_write,
      .flush = physical_flush,
  };
  h2_iostreamikcp_filter_init(&transport.filter);
  puts("H2_LOADER_READY board=jieli_ac791n_devkit target=wl82 transport=iostreamikcp-usb0");
#ifndef H2_JIELI_BLE_DIAG_QUIET_UART
  uint32_t next_ready_diag_ms = 0u;
#endif
  while (!stop_requested) {
    int rc;
    uint32_t now = timer_get_ms();
#ifndef H2_JIELI_BLE_DIAG_QUIET_UART
    if (transport.stream == NULL &&
        (next_ready_diag_ms == 0u || (int32_t)(now - next_ready_diag_ms) >= 0)) {
      usb_diag_write(
          "H2_LOADER_READY board=jieli_ac791n_devkit target=wl82 "
          "transport=iostreamikcp-usb0\r\n");
      next_ready_diag_ms = now + 1000u;
    }
#endif
    if (transport.close_pending && transport.pending_conv == 0u) {
      deactivate_current(&transport);
      continue;
    }
    if (transport.stream != NULL && transport.last_frame_ms != 0u &&
        now - transport.last_frame_ms >= H2_SESSION_IDLE_TIMEOUT_MS) {
      usb_diag_write("H2_JIELI_SESSION_EXPIRED\r\n");
      deactivate_current(&transport);
      h2_iostreamikcp_filter_init(&transport.filter);
      continue;
    }
    if (transport.pending_conv != 0u) {
      h2_loader_command_config_t command_config = command->config;
      command_config.io.user = &transport;
      command_config.io.vtable = &command_io_vtable;
      rc = h2_loader_command_init(command, &command_config);
      if (rc == H2_PAL_OK) rc = activate_pending(&transport);
    } else if (transport.stream == NULL) {
      rc = poll_physical(&transport, 50u);
    } else {
      rc = h2_loader_command_poll(command, 50u);
    }
    if (rc == H2_PAL_OK || rc == H2_PAL_ERR_TIMEOUT ||
        rc == H2_PAL_ERR_WOULD_BLOCK ||
        (rc == H2_PAL_ERR_CLOSED && transport.replacement_pending)) {
      continue;
    }
    char line[96];
    (void)snprintf(
        line, sizeof(line),
        "H2_JIELI_LOADER_ERROR step=command code=%d\r\n", rc);
    usb_diag_write(line);
    /* A framing, filesystem, or overflow error invalidates the current KCP
     * command stream. Drop it completely so a fresh Host session can recover
     * immediately instead of parsing queued payload bytes as shell commands. */
    deactivate_current(&transport);
    transport.pending_conv = 0u;
    h2_iostreamikcp_filter_init(&transport.filter);
  }
  deactivate_current(&transport);
  return H2_PAL_OK;
}

static int stop_serve(void *user) {
  (void)user;
  stop_requested = 1;
  return H2_PAL_OK;
}

static void loader_task(void *private_data) {
  (void)private_data;
  /* Let USB CDC and storage settle before the first diagnostic. */
  os_time_dly(10u);
  char boot_line[96];
  (void)snprintf(
      boot_line, sizeof(boot_line),
      "H2_JIELI_LOADER_BOOT reset_reason=0x%x next=runtime_config\r\n",
      (unsigned)system_reset_reason_get());
  usb_diag_write(boot_line);
  if (crash_recovery_active) {
    usb_diag_write(
        "H2_JIELI_CRASH_RECOVERY state=active transport=board-console "
        "app_boot=blocked\r\n");
  }
  h2_runtime_config_t runtime_config;
  h2_runtime_t *runtime = NULL;
  int rc;
  uint32_t runtime_config_attempt = 0u;
  for (;;) {
    ++runtime_config_attempt;
    rc = h2_jieli_ac791n_devkit_runtime_config(&runtime_config);
    if (rc == H2_PAL_OK) break;
    printf(
        "H2_JIELI_LOADER_WAIT step=runtime_config sd_stage=%s "
        "attempt=%u code=%d\r\n",
        h2_jieli_ac791n_devkit_sd_fs_last_stage(),
        (unsigned)runtime_config_attempt, rc);
    char sd_diagnostic[384];
    char line[544];
    (void)h2_jieli_ac791n_devkit_sd_fs_diagnostic(
        sd_diagnostic, sizeof(sd_diagnostic));
    (void)snprintf(
        line, sizeof(line),
        "H2_JIELI_LOADER_WAIT step=runtime_config sd_stage=%s "
        "attempt=%u code=%d %s\r\n",
        h2_jieli_ac791n_devkit_sd_fs_last_stage(),
        (unsigned)runtime_config_attempt, rc, sd_diagnostic);
    usb_diag_write(line);
    if (rc != H2_PAL_ERR_UNAVAILABLE && rc != H2_PAL_ERR_IO) {
      usb_diag_error_forever("runtime_config", rc);
    }
    os_time_dly(50u);
  }
  usb_diag_write("H2_JIELI_LOADER_OK step=runtime_config\r\n");
  usb_diag_write("H2_JIELI_LOADER_ENTER step=coredump_flush\r\n");
  rc = h2_jieli_wl82_coredump_flush_pending();
  if (rc < 0) {
    usb_diag_write("H2_JIELI_LOADER_ERROR step=coredump_flush code=-1\r\n");
  } else if (rc > 0) {
    usb_diag_write("H2_JIELI_LOADER_OK step=coredump_flush stored=1\r\n");
  } else {
    usb_diag_write("H2_JIELI_LOADER_OK step=coredump_flush stored=0\r\n");
  }
  usb_diag_write("H2_JIELI_LOADER_ENTER step=loader_platform\r\n");
  rc = h2_jieli_loader_platform_init(
      runtime_config.fs, runtime_config.pref, runtime_config.mem);
  if (rc != H2_PAL_OK) {
    printf("H2_JIELI_LOADER_ERROR step=loader_platform code=%d\r\n", rc);
    usb_diag_error_forever("loader_platform", rc);
  }
  usb_diag_write("H2_JIELI_LOADER_OK step=loader_platform\r\n");
  runtime_config.power = h2_jieli_loader_power_api();
  usb_diag_write("H2_JIELI_LOADER_ENTER step=runtime_init\r\n");
  rc = h2_runtime_init(&runtime_config, &runtime);
  if (rc != H2_PAL_OK) {
    printf("H2_JIELI_LOADER_ERROR step=runtime_init code=%d\r\n", rc);
    usb_diag_error_forever("runtime_init", rc);
  }
  usb_diag_write("H2_JIELI_LOADER_OK step=runtime_init\r\n");

  h2_pal_firmware_info_t firmware_info;
  usb_diag_write("H2_JIELI_LOADER_ENTER step=firmware_info\r\n");
  rc = h2_pal_firmware_info_get_current(
      runtime->firmware_info, &firmware_info);
  if (rc != H2_PAL_OK) {
    printf("H2_JIELI_LOADER_ERROR step=firmware_info code=%d\r\n", rc);
    usb_diag_error_forever("firmware_info", rc);
  }
  usb_diag_write("H2_JIELI_LOADER_OK step=firmware_info\r\n");

  stop_requested = 0;
  memset(&digest, 0, sizeof(digest));
  h2loader_app_config_t config;
  memset(&config, 0, sizeof(config));
  config.loader.package.fs = runtime->fs;
  config.loader.package.disk = runtime->disk;
  config.loader.package.digest = (h2_loader_digest_api_t){
      .user = &digest,
      .start = digest_start,
      .update = digest_update,
      .finish = digest_finish,
      .abort = digest_abort,
  };
  config.loader.package.clear_data = clear_data;
  config.loader.package.clear_data_user = (void *)runtime->fs;
  config.loader.package.app_entry_path = H2_JIELI_APP_ENTRY_PATH;
  config.loader.package.image_reader = h2_jieli_loader_image_reader();
  config.loader.package.image_writer = h2_jieli_loader_image_writer();
  config.loader.board = runtime->board;
  config.loader.target = runtime->target;
  config.loader.device_uid = h2_jieli_ac791n_devkit_device_uid();
  config.loader.h2loader_partition_id = H2_JIELI_PARTITION_LOADER;
  config.loader.app_partition_id = H2_JIELI_PARTITION_APP;
  config.loader.hardware_capabilities =
      H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_BLE;
  config.loader.on_event = loader_startup_event;
  rc = current_loader_identity(
      runtime, firmware_info.version, &config.loader.active_identity);
  if (rc != H2_PAL_OK) {
    printf("H2_JIELI_LOADER_ERROR step=active_identity code=%d\r\n", rc);
    usb_diag_error_forever("active_identity", rc);
  }
  config.loader.confirm_active_image =
      h2_jieli_loader_confirm_active_image;
  config.command.digest = config.loader.package.digest;
  config.command.now_ms = now_ms64;
  config.command.sleep_ms = sleep_ms;
  config.command.io.user = &transport;
  config.command.io.vtable = &command_io_vtable;
  config.command.coredump_partition_id = H2_JIELI_PARTITION_COREDUMP;
  config.user = runtime;
  config.prepare = prepare_loader;
  config.stop_serve = stop_serve;
  config.serve = serve_loader;

  /* Preference tracing is useful during filesystem bring-up but it shares the
   * USB0 CDC channel with iKCP. Keep normal Loader builds limited to the
   * transactional install and boot diagnostics below. */
  h2_jieli_ac791n_devkit_pref_set_diagnostic(NULL);
  usb_diag_write("H2_JIELI_LOADER_ENTER step=app_run\r\n");
  /* A failing optional transport must never take the recovery transport down
   * with it.  After a watchdog/assert reset, keep UART Loader command mode
   * alive and skip BLE until a repaired Loader is installed. */
  const h2loader_app_command_service_api_t *additional_command_service =
      crash_recovery_active ? NULL : h2loader_bleikcp_command_service();
  rc = h2loader_app_run_with_command_service(
      runtime, &config, additional_command_service);
  printf("H2_JIELI_LOADER_EXIT code=%d\r\n", rc);
  for (;;) os_time_dly(100u);
}

void app_main(void) {
  usb_tx_ready = 0;
  usb_debug_locked = 0;
  const uint32_t reset_reason = (uint32_t)system_reset_reason_get();
  crash_recovery_active =
      (reset_reason & SYS_RST_WDT) != 0u ||
      h2_jieli_wl82_take_crash_pending();
  if (os_mutex_create(&usb_tx_mutex) != OS_NO_ERR) {
    puts("H2_JIELI_LOADER_ERROR step=usb_sync_create");
    return;
  }
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
  if (h2_jieli_ac791n_devkit_console_start() != H2_PAL_OK) {
    puts("H2_JIELI_LOADER_ERROR step=uart1_start");
    return;
  }
  usb_tx_ready = 1;
  usb_diag_write(
      "H2_JIELI_LOADER_TRANSPORT_READY port=uart1 tx=PB3 rx=PA6 "
      "baud=460800 protocol=iostreamikcp\r\n");
#else
  if (h2_jieli_ac791n_devkit_usb_debug_start() != H2_PAL_OK) {
    puts("H2_JIELI_LOADER_ERROR step=usb0_cdc_start");
    return;
  }
  usb_tx_ready = 1;
  usb_diag_write(
      "H2_JIELI_LOADER_USB_READY port=usb0 class=cdc\r\n");
  usb_diag_write(
      "H2_JIELI_LOADER_TRANSPORT_READY port=usb0 protocol=iostreamikcp\r\n");
#endif
  if (os_task_create(
          loader_task, NULL, 10, H2_LOADER_TASK_STACK_SIZE, 0,
          "h2loader") != OS_NO_ERR) {
    puts("H2_JIELI_LOADER_ERROR step=task_create");
    usb_diag_error_forever("task_create", H2_PAL_ERR_NO_MEMORY);
  }
}
