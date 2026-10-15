#include "h2_esp_board_display_config_internal.h"

#include <limits.h>

int h2_esp_board_display_config_is_valid(
    const h2_esp_board_display_config_t *config) {
    if (config == NULL || config->pclk_hz > (uint32_t)INT_MAX) {
        return 0;
    }
    uint32_t rows = config->dma_buffer_rows;
    if (rows != 0u && (rows < 8u || rows > 64u || (rows & (rows - 1u)) != 0u)) {
        return 0;
    }
    return 1;
}

int h2_esp_board_display_config_may_apply(int already_initialized) {
    return already_initialized == 0;
}

uint32_t h2_esp_board_display_dma_rows(
    const h2_esp_board_display_config_t *config, uint32_t default_rows) {
    if (!h2_esp_board_display_config_is_valid(config) || default_rows < 8u ||
        default_rows > 64u || (default_rows & (default_rows - 1u)) != 0u) {
        return 0u;
    }
    return config->dma_buffer_rows != 0u ? config->dma_buffer_rows : default_rows;
}
