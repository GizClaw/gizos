#include "h2_smoke_display.h"

#include <stdint.h>
#include <stdlib.h>

#define H2_SMOKE_DISPLAY_ROWS 16u

int h2_smoke_display_run(h2_runtime_t *runtime) {
    if (runtime == NULL || runtime->display == NULL) {
        return H2_DISPLAY_ERR_INVALID_ARG;
    }

    int rc = h2_pal_display_open(runtime->display);
    if (rc != H2_DISPLAY_OK) {
        return rc;
    }

    h2_display_info_t info;
    rc = h2_pal_display_get_info(runtime->display, &info);
    if (rc != H2_DISPLAY_OK || info.width <= 0 || info.height <= 0) {
        return rc == H2_DISPLAY_OK ? H2_DISPLAY_ERR_INVALID_ARG : rc;
    }
    (void)h2_pal_display_set_brightness_percent(runtime->display, 90u);

    static const uint16_t colors[] = {
        0xffffu,
        0xffe0u,
        0x07ffu,
        0x07e0u,
        0xf81fu,
        0xf800u,
        0x001fu,
        0x0000u,
    };
    uint16_t *row = (uint16_t *)malloc((size_t)info.width * H2_SMOKE_DISPLAY_ROWS * sizeof(uint16_t));
    if (row == NULL) {
        return H2_DISPLAY_ERR_NO_MEMORY;
    }

    for (int y = 0; y < info.height; y += (int)H2_SMOKE_DISPLAY_ROWS) {
        int rows = info.height - y;
        if (rows > (int)H2_SMOKE_DISPLAY_ROWS) {
            rows = (int)H2_SMOKE_DISPLAY_ROWS;
        }
        for (int yy = 0; yy < rows; ++yy) {
            for (int x = 0; x < info.width; ++x) {
                size_t color_index = ((size_t)x * (sizeof(colors) / sizeof(colors[0]))) / (size_t)info.width;
                row[(size_t)yy * (size_t)info.width + (size_t)x] = colors[color_index];
            }
        }
        h2_display_rect_t rect = {
            .x = 0,
            .y = y,
            .width = info.width,
            .height = rows,
        };
        rc = h2_pal_display_draw_bitmap(
            runtime->display,
            &rect,
            row,
            (size_t)info.width * sizeof(uint16_t),
            H2_DISPLAY_PIXEL_RGB565);
        if (rc != H2_DISPLAY_OK) {
            free(row);
            return rc;
        }
    }
    free(row);
    return h2_pal_display_present(runtime->display);
}
