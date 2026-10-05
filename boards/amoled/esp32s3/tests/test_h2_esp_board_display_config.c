#include "h2_esp_board.h"
#include "../src/h2_esp_board_display_config_internal.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>

int main(void) {
    h2_esp_board_display_config_t config = {
        .pclk_hz = 0u,
    };
    assert(h2_esp_board_display_config_is_valid(&config));
    assert(h2_esp_board_display_config_is_valid(NULL) == 0);
    assert(h2_esp_board_display_dma_rows(&config, 64u) == 64u);
    assert(h2_esp_board_display_dma_rows(NULL, 64u) == 0u);

    config.pclk_hz = (uint32_t)INT_MAX;
    assert(h2_esp_board_display_config_is_valid(&config));
    config.pclk_hz = (uint32_t)INT_MAX + 1u;
    assert(h2_esp_board_display_config_is_valid(&config) == 0);

    config.pclk_hz = 0u;
    const uint32_t valid_rows[] = {0u, 8u, 16u, 32u, 64u};
    for (unsigned i = 0; i < sizeof(valid_rows) / sizeof(valid_rows[0]); ++i) {
        config.dma_buffer_rows = valid_rows[i];
        assert(h2_esp_board_display_config_is_valid(&config));
        assert(h2_esp_board_display_dma_rows(&config, 64u) ==
               (valid_rows[i] == 0u ? 64u : valid_rows[i]));
    }
    const uint32_t invalid_rows[] = {1u, 7u, 9u, 63u, 65u, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(invalid_rows) / sizeof(invalid_rows[0]); ++i) {
        config.dma_buffer_rows = invalid_rows[i];
        assert(h2_esp_board_display_config_is_valid(&config) == 0);
        assert(h2_esp_board_display_dma_rows(&config, 64u) == 0u);
    }

    assert(h2_esp_board_display_config_may_apply(0));
    assert(h2_esp_board_display_config_may_apply(1) == 0);
    config.dma_buffer_rows = 0u;
    assert(h2_esp_board_display_dma_rows(&config, 0u) == 0u);
    assert(h2_esp_board_display_dma_rows(&config, 7u) == 0u);
    assert(h2_esp_board_display_dma_rows(&config, 65u) == 0u);
    puts("AMOLED_DMA_CONFIG_PASS default_rows=64 options=8,16,32,64 invalid_rows=0");
    return 0;
}
