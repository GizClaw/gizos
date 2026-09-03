#include "h2_esp_board.h"

#include <limits.h>

int h2_esp_board_display_config_is_valid(
    const h2_esp_board_display_config_t *config) {
    if (config == NULL || config->pclk_hz > (uint32_t)INT_MAX) {
        return 0;
    }
    return 1;
}

int h2_esp_board_display_config_may_apply(int already_initialized) {
    return already_initialized == 0;
}
