#include "app_config.h"
#include "asm/uart.h"
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
  int count = dev_read(console, buffer, (u32)size);
  if (count == UART_RECV_TIMEOUT) return 0;
  if (count == UART_CIRCULAR_BUFFER_WRITE_OVERLAY) {
    (void)dev_ioctl(console, UART_FLUSH, 0);
    return H2_PAL_ERR_IO;
  }
  return count < 0 ? H2_PAL_ERR_IO : count;
}

int h2_jieli_ac791n_devkit_console_write(
    const void *buffer, size_t size, uint32_t timeout_ms) {
  if (console == NULL) return H2_PAL_ERR_UNAVAILABLE;
  if ((buffer == NULL && size != 0) || size > INT_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  uint32_t ticks = timeout_ms / 10u + (timeout_ms % 10u != 0u);
  if (ticks == 0) ticks = 1;
  if (os_mutex_pend(&tx_mutex, (int)ticks) != OS_NO_ERR)
    return H2_PAL_ERR_TIMEOUT;
  size_t sent = 0;
  int result = 0;
  while (sent < size) {
    size_t take = size - sent;
    if (take > sizeof(tx_chunk)) take = sizeof(tx_chunk);
    memcpy(tx_chunk, (const unsigned char *)buffer + sent, take);
    int count = dev_write(console, tx_chunk, (u32)take);
    if (count <= 0 || (size_t)count > take) {
      result = H2_PAL_ERR_IO;
      break;
    }
    sent += (size_t)count;
  }
  (void)os_mutex_post(&tx_mutex);
  return result != 0 ? result : (int)sent;
}

/* Called by the SDK printf drain. Its task can run without any Loader/App
 * transport initialized, and cannot split a protocol write into two pieces. */
u32 h2_jieli_uart1_debug_write(u8 *buffer, u32 size) {
  int count = h2_jieli_ac791n_devkit_console_write(buffer, size, 100);
  return count > 0 ? (u32)count : 0;
}
