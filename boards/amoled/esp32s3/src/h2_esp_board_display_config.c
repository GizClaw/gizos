#include "h2_esp_board.h"

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
