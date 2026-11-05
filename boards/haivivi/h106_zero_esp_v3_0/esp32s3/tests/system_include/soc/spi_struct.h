#ifndef H2_TEST_SHADOW_SOC_SPI_STRUCT_H
#define H2_TEST_SHADOW_SOC_SPI_STRUCT_H
#include <stdint.h>
/* Only the fields the display stall diagnostics read. */
typedef struct {
  struct {
    uint32_t usr;
  } cmd;
  struct {
    uint32_t val;
  } dma_int_raw;
  struct {
    uint32_t val;
  } dma_int_ena;
  struct {
    uint32_t val;
  } dma_int_st;
} spi_dev_t;
extern spi_dev_t GPSPI2;
#endif
