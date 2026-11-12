#ifndef H2_SPI_NAND_FLASH_USAGE_H
#define H2_SPI_NAND_FLASH_USAGE_H

#include "spi_nand_flash.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns mapped logical bytes and usable FTL capacity under the device mutex.
 * Performs no media I/O or mutation. The initialized handle and outputs are
 * borrowed for the call; the caller must exclude concurrent device teardown.
 * Waits at most one second for the device mutex. Returns ESP_ERR_TIMEOUT on
 * contention, ESP_ERR_INVALID_ARG for NULL arguments, or ESP_ERR_INVALID_STATE
 * for unavailable or inconsistent Dhara state.
 */
esp_err_t h2_spi_nand_flash_get_usage(spi_nand_flash_device_t* handle,
                                      uint64_t* used_bytes,
                                      uint64_t* total_bytes);

#ifdef __cplusplus
}
#endif

#endif
