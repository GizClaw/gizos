#include "h2_zero_bk_spi.h"

#ifdef H2_ZERO_BK_SPI_HOST_TEST
#include "h2_zero_bk_spi_test_sdk.h"
#else
#include <driver/spi.h>
#include <modules/pm.h>
#include <os/os.h>
#endif

/* Runtime configuration initializes this once before exposing peripherals.
 * The SDK routes every SPI TX DMA completion through one global SPI id, so
 * LCD SPI0 and WS2812 SPI1 must not have overlapping DMA transactions. */
static beken_mutex_t s_dma_mutex;

int h2_zero_bk_spi_init(void) {
  return s_dma_mutex != NULL ? kNoErr : rtos_init_mutex(&s_dma_mutex);
}

static int spi_write(uint32_t port, const void* data, uint32_t size, int dma) {
  if (s_dma_mutex == NULL || data == NULL || size == 0u) {
    return kGeneralErr;
  }
#if !defined(CONFIG_SPI_DMA) || !CONFIG_SPI_DMA
  if (dma) return kGeneralErr;
#endif
  int rc = rtos_lock_mutex(&s_dma_mutex);
  if (rc != kNoErr) {
    return rc;
  }
  /* The SDK may suspend peripherals while a synchronous caller waits for
   * its IRQ. The board owns this shared SPI vote; serialize CPU and DMA
   * writes so one writer cannot release another writer's awake lease. */
  rc = bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_SPI_1, 0u, 0u);
  if (rc != kNoErr) {
    (void)rtos_unlock_mutex(&s_dma_mutex);
    return rc;
  }
#if defined(CONFIG_SPI_DMA) && CONFIG_SPI_DMA
  if (dma)
    rc = bk_spi_dma_write_bytes((spi_id_t)port, data, size);
  else
#endif
    rc = bk_spi_write_bytes((spi_id_t)port, data, size);
  /* Both SDK writers return after their completion/FIFO drain boundary. */
  const int sleep_rc =
      bk_pm_module_vote_sleep_ctrl(PM_SLEEP_MODULE_NAME_SPI_1, 1u, 0u);
  const int unlock_rc = rtos_unlock_mutex(&s_dma_mutex);
  const int result = rc != kNoErr         ? rc
                     : sleep_rc != kNoErr ? sleep_rc
                                          : unlock_rc;
  return result;
}

int h2_zero_bk_spi_write(uint32_t port, const void* data, uint32_t size) {
  return spi_write(port, data, size, 0);
}

int h2_zero_bk_spi_dma_write(uint32_t port, const void* data, uint32_t size) {
  return spi_write(port, data, size, 1);
}
