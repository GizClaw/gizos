#ifdef H2_BK_UART_IO_TEST
#include "h2_bk_uart_io_test_sdk.h"
#else
#include "h2_bk_platform_core.h"
#endif

#if CONFIG_SYS_PRINT_DEV_UART
#ifndef H2_BK_UART_IO_TEST
#include "common/bk_include.h"
#include "bk_private/bk_uart.h"
#include "components/shell_task.h"
#include "driver/uart.h"
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
static beken_mutex_t s_direct_write_mutex;
static const uart_id_t s_direct_port = CONFIG_UART_PRINT_PORT;

static void direct_rx_isr(uart_id_t id, void *user) {
  (void)user;
  uint8_t byte;
  while (uart_read_byte_ex(id, &byte) != -1) {
    size_t next = (s_direct_head + 1u) % H2_BK_DIRECT_RX_CAPACITY;
    if (next == s_direct_tail) s_direct_overflow = 1;
    else { s_direct_rx[s_direct_head] = byte; s_direct_head = next; }
  }
}
static h2_pal_result_t direct_configure(void *user, const h2_pal_uart_io_stream_config_t *config) {
  (void)user;
  if (!config || config->baud_rate != CONFIG_UART_PRINT_BAUD_RATE ||
      config->data_bits != 8u || config->stop_bits != 1u ||
      config->parity != H2_PAL_UART_PARITY_NONE ||
      config->flow_control != H2_PAL_UART_FLOW_CONTROL_NONE) return H2_PAL_ERR_INVALID_ARG;
  if (s_direct_initialized) return H2_PAL_OK;
  if (!shell_uart.dev_drv || !shell_uart.dev_drv->io_ctrl) return H2_PAL_ERR_INVALID_STATE;
  if (rtos_init_mutex(&s_direct_write_mutex) != kNoErr) return H2_PAL_ERR_IO;
  s_direct_head = s_direct_tail = 0u;
  s_direct_overflow = 0;
  if (bk_uart_set_baud_rate(s_direct_port, config->baud_rate) != BK_OK ||
      bk_uart_take_rx_isr(s_direct_port, direct_rx_isr, NULL) != BK_OK) {
    (void)rtos_deinit_mutex(&s_direct_write_mutex);
    return H2_PAL_ERR_IO;
  }
  (void)bk_uart_enable_rx_interrupt(s_direct_port);
  s_direct_initialized = 1;
  return H2_PAL_OK;
}
static h2_pal_result_t direct_read(void *user, void *buffer, size_t len, size_t *out_read, uint32_t timeout_ms) {
  (void)user;
  if (!buffer || !out_read) return H2_PAL_ERR_INVALID_ARG;
  *out_read = 0;
  if (!s_direct_initialized) return H2_PAL_ERR_CLOSED;
  uint32_t started = rtos_get_time();
  do {
    uint32_t level = rtos_enter_critical();
    /* Short packets do not reliably raise RX-finish on this chip. */
    direct_rx_isr(s_direct_port, NULL);
    int overflow = s_direct_overflow;
    s_direct_overflow = 0;
    while (*out_read < len && s_direct_tail != s_direct_head) {
      ((uint8_t *)buffer)[(*out_read)++] = s_direct_rx[s_direct_tail];
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
/* All retries share one deadline, including contention and console drain. */
static h2_pal_result_t direct_wait(uint32_t started, uint32_t timeout_ms) {
  if (timeout_ms == 0u) return H2_PAL_ERR_WOULD_BLOCK;
  if (timeout_ms != UINT32_MAX &&
      (uint32_t)(rtos_get_time() - started) >= timeout_ms)
    return H2_PAL_ERR_TIMEOUT;
  rtos_delay_milliseconds(1u);
  return H2_PAL_OK;
}

static h2_pal_result_t direct_write(void *user, const void *buffer, size_t len,
                                    size_t *out_written, uint32_t timeout_ms) {
  (void)user;
  if (!buffer || !out_written || len > UINT32_MAX) return H2_PAL_ERR_INVALID_ARG;
  *out_written = 0;
  if (!s_direct_initialized) return H2_PAL_ERR_CLOSED;
  if (len == 0u) return H2_PAL_OK;
  uint32_t started = rtos_get_time();
  h2_pal_result_t result = H2_PAL_OK;
  while (rtos_trylock_mutex(&s_direct_write_mutex) != kNoErr) {
    result = direct_wait(started, timeout_ms);
    if (result != H2_PAL_OK) return result;
  }

  /* Never call shell_log_flush: it can wait without a deadline. Suspend only
   * after the FIFO drains, with interrupts masked across the check and handoff.
   * SDK TX_SUSPEND then performs only its fixed one-character settling delay. */
  int suspended = 0;
  while (!suspended) {
    uint32_t level = rtos_enter_critical();
    if (bk_uart_is_tx_over(s_direct_port)) {
      suspended = shell_uart.dev_drv->io_ctrl(
          &shell_uart, SHELL_IO_CTRL_TX_SUSPEND, NULL);
      if (suspended) (void)bk_uart_set_enable_tx(s_direct_port, true);
      else result = H2_PAL_ERR_IO;
    }
    rtos_exit_critical(level);
    if (suspended || result != H2_PAL_OK) break;
    result = direct_wait(started, timeout_ms);
    if (result != H2_PAL_OK) break;
  }
  while (suspended && result == H2_PAL_OK && *out_written < len) {
    /* With shell TX suspended and this mutex held there is no competing FIFO
     * writer. A ready-checked single-byte write cannot enter SDK's full-FIFO
     * busy wait. Count bytes accepted by the UART, not physical wire drain. */
    uint32_t level = rtos_enter_critical();
    int ready = uart_write_ready(s_direct_port) == BK_OK;
    if (ready) {
      if (uart_write_byte(s_direct_port,
                          ((const uint8_t *)buffer)[*out_written]) == BK_OK)
        ++*out_written;
      else result = H2_PAL_ERR_IO;
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
  if (suspended)
    (void)shell_uart.dev_drv->io_ctrl(&shell_uart, SHELL_IO_CTRL_TX_RESUME, NULL);
  (void)rtos_unlock_mutex(&s_direct_write_mutex);
  return result;
}
static h2_pal_result_t direct_flush(void *user) {
  (void)user;
  return s_direct_initialized ? H2_PAL_OK : H2_PAL_ERR_CLOSED;
}
static const h2_pal_uart_io_stream_vtable_t s_direct_vtable = {
  .configure = direct_configure, .read = direct_read,
  .write = direct_write, .flush = direct_flush,
};
static const h2_pal_uart_io_stream_api_t s_direct_api = {.vtable = &s_direct_vtable};
const h2_pal_uart_io_stream_api_t *h2_bk_platform_uart_io_stream_api(void) { return &s_direct_api; }
void h2_bk_platform_uart_io_stream_deinit(void) {
  if (s_direct_initialized) {
    (void)bk_uart_disable_rx_interrupt(s_direct_port);
    (void)bk_uart_recover_rx_isr(s_direct_port);
    s_direct_initialized = 0;
    (void)rtos_deinit_mutex(&s_direct_write_mutex);
  }
}
#else
/* This provider requires an AP-owned physical UART. No CP tunnel fallback. */
const h2_pal_uart_io_stream_api_t *h2_bk_platform_uart_io_stream_api(void) {
  return NULL;
}
void h2_bk_platform_uart_io_stream_deinit(void) {}
#endif
