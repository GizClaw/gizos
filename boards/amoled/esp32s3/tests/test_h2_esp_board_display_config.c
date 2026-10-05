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

    config.pclk_hz = 0u;
    const uint32_t valid_rows[] = {0u, 8u, 16u, 32u, 64u};
    for (unsigned i = 0; i < sizeof(valid_rows) / sizeof(valid_rows[0]); ++i) {
        config.dma_buffer_rows = valid_rows[i];
        assert(h2_esp_board_display_config_is_valid(&config));
    }
    const uint32_t invalid_rows[] = {1u, 7u, 9u, 63u, 65u, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(invalid_rows) / sizeof(invalid_rows[0]); ++i) {
        config.dma_buffer_rows = invalid_rows[i];
        assert(h2_esp_board_display_config_is_valid(&config) == 0);
    }

    assert(h2_esp_board_display_config_may_apply(0));
    assert(h2_esp_board_display_config_may_apply(1) == 0);
    return 0;
}
