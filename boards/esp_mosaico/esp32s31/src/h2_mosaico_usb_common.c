#include "h2_mosaico_usb_transport.h"
#ifdef H2_MOSAICO_USB_HOST_TEST
#include "h2_mosaico_usb_test_sdk.h"
#else
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tusb.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif
#include <stdbool.h>
#include <stdatomic.h>

/* Boot-lifetime driver and lock. Initialization is called serially at startup;
 * failure retains completed stages and can be retried by that same caller. */
static bool driver_ready, cdc_ready;
static StaticSemaphore_t tx_storage;
static SemaphoreHandle_t tx_lock;
static atomic_bool failed = false;
static atomic_bool report_closed = false;
static atomic_bool disconnected = false;
static atomic_int dtr_state = -1; /* -1: no host control-line request yet. */
/* DTR is a host control signal, not endpoint readiness. Host Serial Core
 * deliberately deasserts DTR/RTS on open; keep the data path usable then.
 * A DTR drop still ends the old session and discards its buffered bytes. */
static void line_state_changed(int interface, cdcacm_event_t* event) {
  (void)interface;
  /* TinyUSB invokes this for every control-line request, not just edges.
   * The first low request clears boot-time bytes; later repeated DTR-low /
   * RTS-only changes must not terminate the new session. */
  const bool dtr = event->line_state_changed_data.dtr;
  const int previous = atomic_exchange(&dtr_state, dtr ? 1 : 0);
  /* A repeated low request is also a reopen boundary after a failed frame.
   * Healthy low-DTR clients may still change RTS without losing their stream.
   */
  if (!dtr && (previous != 0 || atomic_load(&failed))) {
    atomic_store(&disconnected, true);
    atomic_store(&failed, true);
    atomic_store(&report_closed, true);
  }
}

int h2_mosaico_usb_common_init(void) {
  if (!tx_lock) tx_lock = xSemaphoreCreateMutexStatic(&tx_storage);
  if (!tx_lock) return H2_PAL_ERR_NO_MEMORY;
  if (!driver_ready) {
    const tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    if (tinyusb_driver_install(&config) != ESP_OK) return H2_PAL_ERR_IO;
    driver_ready = true;
  }
  if (!cdc_ready) {
    const tinyusb_config_cdcacm_t cdc = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_line_state_changed = line_state_changed};
    if (tinyusb_cdcacm_init(&cdc) != ESP_OK) return H2_PAL_ERR_IO;
    cdc_ready = true;
  }
  return H2_PAL_OK;
}
static uint32_t remaining(int64_t start, uint32_t timeout) {
  uint64_t elapsed = (uint64_t)(esp_timer_get_time() - start) / 1000u;
  return elapsed >= timeout ? 0 : timeout - (uint32_t)elapsed;
}
static TickType_t ticks(uint32_t ms) {
  TickType_t n = pdMS_TO_TICKS(ms);
  return ms && !n ? 1 : n;
}
static int fail_link(void) {
  /* Called with TX ownership. A partial frame cannot be followed by logs.
   * Only a CDC disconnect/reopen reopens this stream, so the host
   * starts a new byte stream before the next handshake. */
  (void)tud_cdc_n_write_clear(0);
  atomic_store(&failed, true);
  atomic_store(&report_closed, true);
  return H2_PAL_ERR_CLOSED;
}
static bool link_closed(void) {
  return atomic_load(&failed) || atomic_load(&disconnected) ||
         !tud_cdc_n_ready(0);
}
/* The pinned blocking SDK flush logs on timeout. Poll its nonblocking path
 * instead: logging while holding TX would reenter this same console/lock. */
static int drain_tx(int64_t start, uint32_t timeout) {
  for (;;) {
    if (link_closed()) return H2_PAL_ERR_CLOSED;
    esp_err_t rc = tinyusb_cdcacm_write_flush(0, 0);
    if (link_closed()) return H2_PAL_ERR_CLOSED;
    if (rc == ESP_OK) return H2_PAL_OK;
    if (rc != ESP_ERR_NOT_FINISHED && rc != ESP_ERR_TIMEOUT)
      return H2_PAL_ERR_IO;
    if (!tud_cdc_n_ready(0) || atomic_load(&disconnected))
      return H2_PAL_ERR_CLOSED;
    if (!remaining(start, timeout)) return H2_PAL_ERR_TIMEOUT;
    vTaskDelay(1);
  }
}
h2_pal_result_t h2_mosaico_usb_write(void* user, const void* data, size_t len,
                                     size_t* out, uint32_t timeout) {
  (void)user;
  if (!out || (!data && len)) return H2_PAL_ERR_INVALID_ARG;
  *out = 0;
  if (!cdc_ready || !tx_lock) return H2_PAL_ERR_INVALID_STATE;
  if (!len) return H2_PAL_OK;
  int64_t start = esp_timer_get_time();
  if (!xSemaphoreTake(tx_lock, ticks(timeout))) return H2_PAL_ERR_TIMEOUT;
  int rc = H2_PAL_OK;
  if (atomic_load(&failed) || atomic_load(&disconnected) ||
      !tud_cdc_n_ready(0)) {
    rc = H2_PAL_ERR_CLOSED;
    goto done;
  }
  while (*out < len) {
    if (link_closed()) {
      rc = fail_link();
      goto done;
    }
    *out +=
        tinyusb_cdcacm_write_queue(0, (const uint8_t*)data + *out, len - *out);
    if (link_closed()) {
      rc = fail_link();
      goto done;
    }
    esp_err_t err = tinyusb_cdcacm_write_flush(0, 0);
    if (link_closed()) {
      rc = fail_link();
      goto done;
    }
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED &&
        err != ESP_ERR_TIMEOUT) {
      rc = fail_link();
      goto done;
    }
    if (*out == len) break;
    if (!tud_cdc_n_ready(0) || !remaining(start, timeout)) {
      rc = fail_link();
      goto done;
    }
    vTaskDelay(1);
  }
  /* Keep ownership until the whole frame has left the TinyUSB queue. */
  if (drain_tx(start, timeout) != H2_PAL_OK) rc = fail_link();
done:
  xSemaphoreGive(tx_lock);
  return rc;
}
h2_pal_result_t h2_mosaico_usb_flush(void* user) {
  (void)user;
  if (!cdc_ready || !tx_lock) return H2_PAL_ERR_INVALID_STATE;
  int64_t start = esp_timer_get_time();
  if (!xSemaphoreTake(tx_lock, ticks(1000))) return H2_PAL_ERR_TIMEOUT;
  int rc = H2_PAL_OK;
  if (atomic_load(&failed) || atomic_load(&disconnected) || !tud_cdc_n_ready(0))
    rc = H2_PAL_ERR_CLOSED;
  else if (drain_tx(start, 1000) != H2_PAL_OK)
    rc = fail_link();
  xSemaphoreGive(tx_lock);
  return rc;
}
h2_pal_result_t h2_mosaico_usb_read(void* user, void* data, size_t len,
                                    size_t* out, uint32_t timeout) {
  (void)user;
  if (!out || (!data && len)) return H2_PAL_ERR_INVALID_ARG;
  *out = 0;
  if (!cdc_ready) return H2_PAL_ERR_INVALID_STATE;
  if (!len) return H2_PAL_OK;
  int64_t start = esp_timer_get_time();
  for (;;) {
    if (!tud_cdc_n_ready(0)) {
      atomic_store(&disconnected, true);
      atomic_store(&failed, true);
      return H2_PAL_ERR_CLOSED;
    }
    if (atomic_exchange(&report_closed, false)) return H2_PAL_ERR_CLOSED;
    if (atomic_exchange(&disconnected, false)) {
      if (!xSemaphoreTake(tx_lock, ticks(remaining(start, timeout)))) {
        atomic_store(&disconnected, true);
        return H2_PAL_ERR_TIMEOUT;
      }
      (void)tud_cdc_n_write_clear(0);
      tud_cdc_n_read_flush(
          0); /* Never feed old-session RX into a new filter. */
      /* A newer callback must keep output gated; never erase its event. */
      atomic_store(&failed, atomic_load(&disconnected));
      xSemaphoreGive(tx_lock);
    }
    if (atomic_load(&failed)) return H2_PAL_ERR_CLOSED;
    if (tinyusb_cdcacm_read(0, data, len, out) != ESP_OK) return H2_PAL_ERR_IO;
    if (*out) return H2_PAL_OK;
    if (!remaining(start, timeout)) return H2_PAL_ERR_TIMEOUT;
    vTaskDelay(1);
  }
}
