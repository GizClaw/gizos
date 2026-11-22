#ifndef H2_ESP_BOARD_DISPLAY_CONFIG_INTERNAL_H
#define H2_ESP_BOARD_DISPLAY_CONFIG_INTERNAL_H

#include "h2_esp_board.h"
#include "hal/h2_pal_display.h"

#define H2_AMOLED_NATIVE_WIDTH 368
#define H2_AMOLED_NATIVE_HEIGHT 448

h2_esp_board_display_orientation_t h2_esp_board_display_orientation(void);

void h2_esp_board_display_extent(h2_esp_board_display_orientation_t orientation,
                                 int32_t* width, int32_t* height);
void h2_esp_board_display_touch_point(
    h2_esp_board_display_orientation_t orientation, int32_t* x, int32_t* y);

/* Pack one clipped logical chunk in native GRAM order. The returned native
 * rectangle and packed big-endian RGB565 data describe the same transfer. */
h2_display_rect_t h2_esp_board_display_pack(
    h2_esp_board_display_orientation_t orientation,
    const h2_display_rect_t* source, const h2_display_rect_t* chunk,
    const void* pixels, size_t stride_bytes, h2_display_pixel_format_t format,
    size_t pixel_size, uint16_t* out);

/* Resolve the validated option against the caller's unchanged DMA default.
 * Invalid configuration/default returns zero, outside every supported chunk. */
uint32_t h2_esp_board_display_dma_rows(
    const h2_esp_board_display_config_t* config, uint32_t default_rows);

#endif
