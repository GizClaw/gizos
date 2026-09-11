#include "h2_esp_board.h"

#include <limits.h>

int h2_esp_board_display_config_is_valid(
    const h2_esp_board_display_config_t *config) {
    if (config == NULL || config->pclk_hz > (uint32_t)INT_MAX ||
        (config->sync_to_te != 0 && config->sync_to_te != 1) ||
        (config->sync_to_te != 0) != (config->te_timeout_ms != 0u) ||
        config->te_timeout_ms > 1000u) {
        return 0;
    }
    return 1;
}

int h2_esp_board_display_config_may_apply(int already_initialized) {
    return already_initialized == 0;
}
