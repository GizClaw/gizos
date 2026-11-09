#ifndef H2_TEST_SHADOW_DRIVER_SPI_MASTER_H
#define H2_TEST_SHADOW_DRIVER_SPI_MASTER_H
#include "esp_err.h"
#include <stddef.h>
typedef enum { SPI1_HOST = 0, SPI2_HOST, SPI3_HOST } spi_host_device_t;
typedef enum { SPI_DMA_DISABLED = 0, SPI_DMA_CH_AUTO = 3 } spi_common_dma_t;
typedef struct {
  int sclk_io_num;
  int mosi_io_num;
  int miso_io_num;
  int quadwp_io_num;
  int quadhd_io_num;
  int max_transfer_sz;
} spi_bus_config_t;
esp_err_t spi_bus_initialize(spi_host_device_t host,
                             const spi_bus_config_t* cfg,
                             spi_common_dma_t dma_chan);
#endif
