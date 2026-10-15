#ifndef H2_ESP_BOARD_DISPLAY_CONFIG_INTERNAL_H
#define H2_ESP_BOARD_DISPLAY_CONFIG_INTERNAL_H

#include "h2_esp_board.h"

/* Resolve the validated option against the caller's unchanged DMA default.
 * Invalid configuration/default returns zero, outside every supported chunk. */
uint32_t h2_esp_board_display_dma_rows(
    const h2_esp_board_display_config_t *config, uint32_t default_rows);

#endif
