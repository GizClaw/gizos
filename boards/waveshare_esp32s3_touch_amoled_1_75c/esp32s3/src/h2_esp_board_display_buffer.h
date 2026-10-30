#ifndef H2_ESP_BOARD_DISPLAY_BUFFER_H
#define H2_ESP_BOARD_DISPLAY_BUFFER_H

#include "h2/pal/hal/h2_pal_display.h"

/* Store RGB565 in panel byte order and return an even-aligned transfer area.
 * The shadow buffer preserves pixels surrounding odd/partial updates. */
int h2_ws175c_display_buffer_update(uint16_t* shadow,
                                    const h2_display_rect_t* rect,
                                    const void* pixels, size_t stride_bytes,
                                    h2_display_pixel_format_t format,
                                    h2_display_rect_t* transfer);

#endif
