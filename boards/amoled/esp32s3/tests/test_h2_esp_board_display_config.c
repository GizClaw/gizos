#include "h2_esp_board.h"

#include <assert.h>
#include <limits.h>

int main(void) {
    h2_esp_board_display_config_t config = {
        .pclk_hz = 0u,
    };
    assert(h2_esp_board_display_config_is_valid(&config));
    assert(h2_esp_board_display_config_is_valid(NULL) == 0);

    config.pclk_hz = (uint32_t)INT_MAX;
    assert(h2_esp_board_display_config_is_valid(&config));
    config.pclk_hz = (uint32_t)INT_MAX + 1u;
    assert(h2_esp_board_display_config_is_valid(&config) == 0);

    assert(h2_esp_board_display_config_may_apply(0));
    assert(h2_esp_board_display_config_may_apply(1) == 0);
    return 0;
}
