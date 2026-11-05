#ifndef H2_ZERO_BK_SPI_H
#define H2_ZERO_BK_SPI_H

#include <stdint.h>

int h2_zero_bk_spi_init(void);
int h2_zero_bk_spi_dma_write(uint32_t port, const void* data, uint32_t size);

#endif
