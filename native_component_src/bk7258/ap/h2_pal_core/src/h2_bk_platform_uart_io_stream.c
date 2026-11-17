#ifdef H2_BK_UART_IO_TEST
#include "h2_bk_uart_io_test_sdk.h"
#else
#include "h2_bk_platform_core.h"
#endif

#include "os/h2_pal_io_stream.h"
#include "h2_bk_console_tx_internal.h"
#include "h2_pal_unsupported.h"

#if CONFIG_SYS_PRINT_DEV_UART
#ifndef H2_BK_UART_IO_TEST
#include "common/bk_include.h"
#include "bk_private/bk_uart.h"
#include "components/shell_task.h"
#include "driver/uart.h"
#include "modules/pm.h"
#include "os/os.h"
#include "shell_drv.h"
#endif

/* The board selects the physical console. RX is owned by this provider so
 * shell command parsing cannot consume protocol bytes. */
#define H2_BK_DIRECT_RX_CAPACITY 8192u
static uint8_t s_direct_rx[H2_BK_DIRECT_RX_CAPACITY];
static size_t s_direct_head, s_direct_tail;
static int s_direct_overflow;
static int s_direct_initialized;
static int s_direct_releasing;
static unsigned s_direct_release_steps;
/* This admission gate outlives management sessions. Console writers must never
 * borrow a mutex that a concurrent management release can destroy. Configure
 * and release take the same gate before changing RX or the UART sleep vote. */
static int s_direct_tx_busy;
static const uart_id_t s_direct_port = CONFIG_UART_PRINT_PORT;
static const pm_sleep_module_name_e s_direct_sleep_module =
    CONFIG_UART_PRINT_PORT == 0   ? PM_SLEEP_MODULE_NAME_UART1
    : CONFIG_UART_PRINT_PORT == 1 ? PM_SLEEP_MODULE_NAME_UART2
                                  : PM_SLEEP_MODULE_NAME_UART3;

/* All retries share one deadline, including contention and console drain. */
static h2_pal_result_t direct_wait(uint32_t started, uint32_t timeout_ms) {
  if (timeout_ms == 0u) return H2_PAL_ERR_WOULD_BLOCK;
  if (timeout_ms != UINT32_MAX &&
      (uint32_t)(rtos_get_time() - started) >= timeout_ms)
    return H2_PAL_ERR_TIMEOUT;
  rtos_delay_milliseconds(1u);
  return H2_PAL_OK;
}
static int direct_trylock(void) {
  uint32_t level = rtos_enter_critical();
  int acquired = !s_direct_tx_busy;
  if (acquired) s_direct_tx_busy = 1;
  rtos_exit_critical(level);
  return acquired;
}
static void direct_unlock(void) {
  uint32_t level = rtos_enter_critical();
  s_direct_tx_busy = 0;
  rtos_exit_critical(level);
}
static int direct_expired(uint32_t started, uint32_t timeout_ms) {
  return timeout_ms != 0u && timeout_ms != UINT32_MAX &&
         (uint32_t)(rtos_get_time() - started) >= timeout_ms;
}
static h2_pal_result_t direct_lock(uint32_t started, uint32_t timeout_ms) {
  while (!direct_trylock()) {
    h2_pal_result_t rc = direct_wait(started, timeout_ms);
    if (rc != H2_PAL_OK) return rc;
  }
  if (direct_expired(started, timeout_ms)) {
    direct_unlock();
    return H2_PAL_ERR_TIMEOUT;
  }
  return H2_PAL_OK;
}

static void direct_rx_isr(uart_id_t id, void* user) {
  (void)user;
  uint8_t byte;
  while (uart_read_byte_ex(id, &byte) != -1) {
    size_t next = (s_direct_head + 1u) % H2_BK_DIRECT_RX_CAPACITY;
    if (next == s_direct_tail)
      s_direct_overflow = 1;
    else {
      s_direct_rx[s_direct_head] = byte;
      s_direct_head = next;
    }
  }
}
static h2_pal_result_t direct_configure_locked(
    const h2_pal_uart_io_stream_config_t* config) {
  if (s_direct_releasing) return H2_PAL_ERR_INVALID_STATE;
  if (s_direct_initialized) return H2_PAL_OK;
  if (!shell_uart.dev_drv || !shell_uart.dev_drv->io_ctrl)
    return H2_PAL_ERR_INVALID_STATE;
  /* Raw UART I/O bypasses shell's wakeup bookkeeping. Its PM callback can
   * otherwise suspend/backup/restore this UART during an accepted write.
   * Own the UART's separate sleep vote, not LOG (ignored by the AP SDK). */
  if (bk_pm_module_vote_sleep_ctrl(s_direct_sleep_module, 0u, 0u) != BK_OK)
    return H2_PAL_ERR_IO;
  s_direct_head = s_direct_tail = 0u;
  s_direct_overflow = 0;
  if (bk_uart_set_baud_rate(s_direct_port, config->baud_rate) != BK_OK ||
      bk_uart_take_rx_isr(s_direct_port, direct_rx_isr, NULL) != BK_OK) {
    (void)bk_pm_module_vote_sleep_ctrl(s_direct_sleep_module, 1u, 0u);
    return H2_PAL_ERR_IO;
  }
  (void)bk_uart_enable_rx_interrupt(s_direct_port);
  s_direct_initialized = 1;
  return H2_PAL_OK;
}
static h2_pal_result_t direct_configure(
    void* user, const h2_pal_uart_io_stream_config_t* config) {
  (void)user;
  if (!config || config->baud_rate != CONFIG_UART_PRINT_BAUD_RATE ||
      config->data_bits != 8u || config->stop_bits != 1u ||
      config->parity != H2_PAL_UART_PARITY_NONE ||
      config->flow_control != H2_PAL_UART_FLOW_CONTROL_NONE)
    return H2_PAL_ERR_INVALID_ARG;
  if (!direct_trylock()) return H2_PAL_ERR_BUSY;
  h2_pal_result_t rc = direct_configure_locked(config);
  direct_unlock();
  return rc;
}
static h2_pal_result_t direct_read(void* user, void* buffer, size_t len,
                                   size_t* out_read, uint32_t timeout_ms) {
  (void)user;
  if (!buffer || !out_read) return H2_PAL_ERR_INVALID_ARG;
  *out_read = 0;
  if (!s_direct_initialized || s_direct_releasing) return H2_PAL_ERR_CLOSED;
  uint32_t started = rtos_get_time();
  do {
    uint32_t level = rtos_enter_critical();
    /* Short packets do not reliably raise RX-finish on this chip. */
    direct_rx_isr(s_direct_port, NULL);
    int overflow = s_direct_overflow;
    s_direct_overflow = 0;
    while (*out_read < len && s_direct_tail != s_direct_head) {
      ((uint8_t*)buffer)[(*out_read)++] = s_direct_rx[s_direct_tail];
      s_direct_tail = (s_direct_tail + 1u) % H2_BK_DIRECT_RX_CAPACITY;
    }
    rtos_exit_critical(level);
    if (overflow) return H2_PAL_ERR_IO;
    if (*out_read) return H2_PAL_OK;
    if (!timeout_ms) return H2_PAL_ERR_WOULD_BLOCK;
    rtos_delay_milliseconds(1u);
  } while ((uint32_t)(rtos_get_time() - started) < timeout_ms);
  return H2_PAL_ERR_TIMEOUT;
}
/* Caller owns the admission gate and the UART wake vote. */
static h2_pal_result_t direct_transmit(const void* buffer, size_t len,
                                       size_t* out_written, uint32_t started,
                                       uint32_t timeout_ms) {
  h2_pal_result_t result = H2_PAL_OK;
  /* Never call shell_log_flush: it can wait without a deadline. Suspend only
   * after the FIFO drains, with interrupts masked across the check and handoff.
   * SDK TX_SUSPEND then performs only its fixed one-character settling delay.
   */
  int suspended = 0;
  while (!suspended) {
    if (direct_expired(started, timeout_ms)) {
      result = H2_PAL_ERR_TIMEOUT;
      break;
    }
    uint32_t level = rtos_enter_critical();
    if (bk_uart_is_tx_over(s_direct_port)) {
      suspended = shell_uart.dev_drv->io_ctrl(&shell_uart,
                                              SHELL_IO_CTRL_TX_SUSPEND, NULL);
      if (!suspended || bk_uart_set_enable_tx(s_direct_port, true) != BK_OK)
        result = H2_PAL_ERR_IO;
    }
    rtos_exit_critical(level);
    if (suspended || result != H2_PAL_OK) break;
    result = direct_wait(started, timeout_ms);
    if (result != H2_PAL_OK) break;
  }
  while (suspended && result == H2_PAL_OK && *out_written < len) {
    if (direct_expired(started, timeout_ms)) {
      result = H2_PAL_ERR_TIMEOUT;
      break;
    }
    /* With shell TX suspended and the gate held there is no competing FIFO
     * writer. A ready-checked single-byte write cannot enter SDK's full-FIFO
     * busy wait. Keep ownership until accepted bytes leave the FIFO. */
    uint32_t level = rtos_enter_critical();
    int ready = uart_write_ready(s_direct_port) == BK_OK;
    if (ready) {
      if (uart_write_byte(s_direct_port,
                          ((const uint8_t*)buffer)[*out_written]) == BK_OK)
        ++*out_written;
      else
        result = H2_PAL_ERR_IO;
    }
    rtos_exit_critical(level);
    if (!ready) {
      if (timeout_ms == 0u && *out_written != 0u) break;
      result = direct_wait(started, timeout_ms);
    } else if (*out_written < len && timeout_ms != 0u &&
               timeout_ms != UINT32_MAX &&
               (uint32_t)(rtos_get_time() - started) >= timeout_ms) {
      result = H2_PAL_ERR_TIMEOUT;
    }
  }
  /* SDK's resumed owner can change TX state. FIFO drain shares the original
   * deadline, including partial writes; success must not hand off queued bytes.
   */
  while (suspended && result == H2_PAL_OK && !bk_uart_is_tx_over(s_direct_port))
    result = direct_wait(started, timeout_ms);
  if (suspended &&
      !shell_uart.dev_drv->io_ctrl(&shell_uart, SHELL_IO_CTRL_TX_RESUME, NULL))
    result = H2_PAL_ERR_IO;
  return result;
}
static h2_pal_result_t direct_write(void* user, const void* buffer, size_t len,
                                    size_t* out_written, uint32_t timeout_ms) {
  (void)user;
  if (!buffer || !out_written || len > UINT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  *out_written = 0u;
  uint32_t started = rtos_get_time();
  h2_pal_result_t rc = direct_lock(started, timeout_ms);
  if (rc != H2_PAL_OK) return rc;
  if (!s_direct_initialized || s_direct_releasing)
    rc = H2_PAL_ERR_CLOSED;
  else if (len != 0u)
    rc = direct_transmit(buffer, len, out_written, started, timeout_ms);
  direct_unlock();
  return rc;
}
h2_pal_result_t h2_bk_console_write(const void* buffer, size_t len,
                                    size_t* out_written, uint32_t timeout_ms) {
  if (!buffer || !out_written || len > UINT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  *out_written = 0u;
  if (len == 0u) return H2_PAL_OK;
  uint32_t started = rtos_get_time();
  h2_pal_result_t rc = direct_lock(started, timeout_ms);
  if (rc != H2_PAL_OK) return rc;
  int temporary_vote = 0;
  if (s_direct_releasing)
    rc = H2_PAL_ERR_CLOSED;
  else if (!shell_uart.dev_drv || !shell_uart.dev_drv->io_ctrl)
    rc = H2_PAL_ERR_INVALID_STATE;
  else if (!s_direct_initialized) {
    if (bk_pm_module_vote_sleep_ctrl(s_direct_sleep_module, 0u, 0u) != BK_OK)
      rc = H2_PAL_ERR_IO;
    else
      temporary_vote = 1;
  }
  if (rc == H2_PAL_OK)
    rc = direct_transmit(buffer, len, out_written, started, timeout_ms);
  if (temporary_vote &&
      bk_pm_module_vote_sleep_ctrl(s_direct_sleep_module, 1u, 0u) != BK_OK)
    rc = H2_PAL_ERR_IO;
  direct_unlock();
  if (rc == H2_PAL_OK && *out_written != len)
    rc = timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_TIMEOUT;
  return rc;
}
static h2_pal_result_t direct_flush(void* user) {
  (void)user;
  return s_direct_initialized && !s_direct_releasing ? H2_PAL_OK
                                                     : H2_PAL_ERR_CLOSED;
}
static h2_pal_result_t direct_release(void* user);
static const h2_pal_uart_io_stream_vtable_t s_direct_vtable = {
    .configure = direct_configure,
    .read = direct_read,
    .write = direct_write,
    .flush = direct_flush,
    .release = direct_release,
};
static const h2_pal_uart_io_stream_api_t s_direct_api = {.vtable =
                                                             &s_direct_vtable};
const h2_pal_uart_io_stream_api_t* h2_bk_platform_uart_io_stream_api(void) {
  return &s_direct_api;
}
void h2_bk_platform_uart_io_stream_deinit(void) { (void)direct_release(NULL); }
static h2_pal_result_t direct_release_locked(void) {
  if (!s_direct_initialized) return H2_PAL_OK;
  /* Stop I/O admission as soon as release begins. Retain completed steps so a
   * failed native cleanup can retry without repeating completed RX cleanup. */
  s_direct_releasing = 1;
  if ((s_direct_release_steps & 1u) == 0u) {
    if (bk_uart_disable_rx_interrupt(s_direct_port) != BK_OK)
      return H2_PAL_ERR_IO;
    s_direct_release_steps |= 1u;
  }
  if ((s_direct_release_steps & 2u) == 0u) {
    if (bk_uart_recover_rx_isr(s_direct_port) != BK_OK) return H2_PAL_ERR_IO;
    s_direct_release_steps |= 2u;
  }
  if (bk_pm_module_vote_sleep_ctrl(s_direct_sleep_module, 1u, 0u) != BK_OK)
    return H2_PAL_ERR_IO;
  s_direct_initialized = 0;
  s_direct_releasing = 0;
  s_direct_release_steps = 0u;
  return H2_PAL_OK;
}
static h2_pal_result_t direct_release(void* user) {
  (void)user;
  if (!direct_trylock()) return H2_PAL_ERR_BUSY;
  h2_pal_result_t rc = direct_release_locked();
  direct_unlock();
  return rc;
}
static h2_pal_result_t stream_start(void* user) {
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = CONFIG_UART_PRINT_BAUD_RATE,
      .data_bits = 8u,
      .stop_bits = 1u,
      .rx_buffer_size = 4096u,
      .tx_buffer_size = 2048u,
  };
  return direct_configure(user, &config);
}
static h2_pal_result_t stream_write(void* user, const void* buffer, size_t len,
                                    size_t* out_written, uint32_t timeout_ms) {
  h2_pal_result_t rc = direct_write(user, buffer, len, out_written, timeout_ms);
  /* Native polling may accept only a prefix. TX ownership has ended, so the
   * caller must not retry that suffix as though it were still one frame. */
  if (rc == H2_PAL_OK && *out_written != len)
    return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_TIMEOUT;
  return rc;
}
static const h2_pal_io_stream_vtable_t s_stream_vtable = {
    .start = stream_start,
    .read = direct_read,
    .write = stream_write,
    .flush = direct_flush,
    .release = direct_release,
};
static const h2_pal_io_stream_api_t s_stream_api = {.vtable = &s_stream_vtable};
const h2_pal_io_stream_api_t* h2_bk_platform_io_stream_api(void) {
  return &s_stream_api;
}
#else
h2_pal_result_t h2_bk_console_write(const void* buffer, size_t len,
                                    size_t* out_written, uint32_t timeout_ms) {
  (void)buffer;
  (void)len;
  (void)timeout_ms;
  if (out_written != NULL) *out_written = 0u;
  return H2_PAL_ERR_UNSUPPORTED;
}
const h2_pal_io_stream_api_t* h2_bk_platform_io_stream_api(void) {
  return h2_pal_unsupported_io_stream_api();
}
/* This provider requires an AP-owned physical UART. No CP tunnel fallback. */
const h2_pal_uart_io_stream_api_t* h2_bk_platform_uart_io_stream_api(void) {
  return NULL;
}
void h2_bk_platform_uart_io_stream_deinit(void) {}
#endif
