#include "h2_zero_bk_spi.h"

#ifdef H2_ZERO_BK_SPI_HOST_TEST
#include "h2_zero_bk_spi_test_sdk.h"
#else
#include <driver/spi.h>
#include <os/os.h>
#endif

/* Runtime configuration initializes this once before exposing peripherals.
 * The SDK routes every SPI TX DMA completion through one global SPI id, so
 * LCD SPI0 and WS2812 SPI1 must not have overlapping DMA transactions. */
static beken_mutex_t s_dma_mutex;

int h2_zero_bk_spi_init(void) {
  return s_dma_mutex != NULL ? kNoErr : rtos_init_mutex(&s_dma_mutex);
}

int h2_zero_bk_spi_dma_write(uint32_t port, const void* data, uint32_t size) {
#if defined(CONFIG_SPI_DMA) && CONFIG_SPI_DMA
  if (s_dma_mutex == NULL || data == NULL || size == 0u) {
    return kGeneralErr;
  }
  int rc = rtos_lock_mutex(&s_dma_mutex);
  if (rc != kNoErr) {
    return rc;
  }
  rc = bk_spi_dma_write_bytes((spi_id_t)port, data, size);
  const int unlock_rc = rtos_unlock_mutex(&s_dma_mutex);
  return rc == kNoErr ? unlock_rc : rc;
#else
  (void)port;
  (void)data;
  (void)size;
  return kGeneralErr;
#endif
}
