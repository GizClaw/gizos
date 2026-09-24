#include "app_config.h"
#include "asm/uart.h"
#include "asm/includes.h"
#include "device/includes.h"
#include "os/os_api.h"
#include "h2_jieli_ac791n_devkit.h"

#include <limits.h>
#include <string.h>

/* Follow the SDK uart_test.c RX ring/start sequence. RX uses DMA; TX uses
 * bounded-size, aligned copies so neither caller alignment nor stack lifetime
 * becomes a driver precondition. No target owns a second UART handle. */
static unsigned char rx_ring[16 * 1024] __attribute__((aligned(32)));
static unsigned char tx_chunk[512] __attribute__((aligned(32)));
static OS_MUTEX tx_mutex;
static void *console;
static int mutex_ready;
static int tx_pending;

h2_pal_result_t h2_jieli_ac791n_devkit_console_start(void) {
  if (console != NULL) return H2_PAL_OK;
  if (!mutex_ready) {
    if (os_mutex_create(&tx_mutex) != OS_NO_ERR) return H2_PAL_ERR_IO;
    mutex_ready = 1;
  }
  void *handle = dev_open("uart1", NULL);
  if (handle == NULL) return H2_PAL_ERR_IO;
  if (dev_ioctl(handle, UART_SET_CIRCULAR_BUFF_ADDR, (u32)rx_ring) != 0 ||
      dev_ioctl(handle, UART_SET_CIRCULAR_BUFF_LENTH, sizeof(rx_ring)) != 0 ||
      dev_ioctl(handle, UART_SET_RECV_BLOCK, 0) != 0 ||
      dev_ioctl(handle, UART_START, 0) != 0) {
    dev_close(handle);
    return H2_PAL_ERR_IO;
  }
  console = handle;
  return H2_PAL_OK;
}

int h2_jieli_ac791n_devkit_console_read(void *buffer, size_t size) {
  if (console == NULL) return H2_PAL_ERR_UNAVAILABLE;
  if (buffer == NULL || size == 0 || size > INT_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  /* Serialize access to the SDK handle with the diagnostic/protocol writer.
   * RX is non-blocking; do not hold this lock while waiting for input. */
  if (os_mutex_accept(&tx_mutex) != OS_NO_ERR) return 0;
  int count = dev_read(console, buffer, (u32)size);
  if (count == UART_CIRCULAR_BUFFER_WRITE_OVERLAY) {
    (void)dev_ioctl(console, UART_FLUSH, 0);
  }
  (void)os_mutex_post(&tx_mutex);
  if (count == UART_RECV_TIMEOUT) return 0;
  if (count == UART_CIRCULAR_BUFFER_WRITE_OVERLAY) {
    return H2_PAL_ERR_IO;
  }
  return count < 0 ? H2_PAL_ERR_IO : count;
}

static int console_tx_complete(void) {
  /* spec_uart.c's non-IRQ send path tests CON0 bit 15 before reusing TXADR.
   * RX interrupts do not clear this flag when TX IRQ is disabled. */
  if (tx_pending && (JL_UART1->CON0 & BIT(15)) != 0u) tx_pending = 0;
  return !tx_pending;
}

static int console_deadline_wait(uint32_t started, uint32_t timeout_ms) {
  const uint32_t elapsed = (uint32_t)(timer_get_ms() - started);
  if (elapsed >= timeout_ms) return 0;
  /* A sub-tick remainder must not be rounded into another blocking tick. */
  if (timeout_ms - elapsed >= 10u) os_time_dly(1u);
  return 1;
}

int h2_jieli_ac791n_devkit_console_write(
    const void *buffer, size_t size, uint32_t timeout_ms) {
  if (console == NULL) return H2_PAL_ERR_UNAVAILABLE;
  if ((buffer == NULL && size != 0) || size > INT_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  if (size == 0u) return 0;
  const uint32_t started = timer_get_ms();
  while (os_mutex_accept(&tx_mutex) != OS_NO_ERR) {
    const uint32_t elapsed = (uint32_t)(timer_get_ms() - started);
    if (elapsed >= timeout_ms) return H2_PAL_ERR_TIMEOUT;
    const uint32_t ticks = (timeout_ms - elapsed) / 10u;
    if (ticks != 0u && os_mutex_pend(&tx_mutex, (int)ticks) == OS_NO_ERR) break;
  }
  size_t sent = 0u;
  int result = 0;
  for (;;) {
    /* The staging bytes remain owned by DMA even if the prior caller timed
     * out. Never overwrite them until hardware reports completion. */
    if (!console_tx_complete()) {
      if (!console_deadline_wait(started, timeout_ms)) {
        result = H2_PAL_ERR_TIMEOUT;
        break;
      }
      continue;
    }
    if (sent == size) break;
    if (timeout_ms != 0u &&
        (uint32_t)(timer_get_ms() - started) >= timeout_ms) {
      result = H2_PAL_ERR_TIMEOUT;
      break;
    }
    size_t take = size - sent;
    if (take > sizeof(tx_chunk)) take = sizeof(tx_chunk);
    memcpy(tx_chunk, (const unsigned char *)buffer + sent, take);
    /* This is the sole uart1 writer. Platform TX IRQ is disabled and the
     * prior DMA is complete, so the SDK neither waits for old TX nor pends
     * its completion semaphore. Its internal writer mutex is uncontended. */
    int count = dev_write(console, tx_chunk, (u32)take);
    if (count > 0) tx_pending = 1;
    if (count <= 0 || (size_t)count > take) {
      result = H2_PAL_ERR_IO;
      break;
    }
    sent += (size_t)count;
  }
  (void)os_mutex_post(&tx_mutex);
  return sent != 0u ? (int)sent : result;
}

/* Called by the SDK printf drain. Its task can run without any Loader/App
 * transport initialized, and cannot split a protocol write into two pieces. */
u32 h2_jieli_uart1_debug_write(u8 *buffer, u32 size) {
  int count = h2_jieli_ac791n_devkit_console_write(buffer, size, 100);
  return count > 0 ? (u32)count : 0;
}
