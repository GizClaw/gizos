#ifndef H2_DISPLAY_ESP_CAPTURE_H
#define H2_DISPLAY_ESP_CAPTURE_H

#include "h2/pal/hal/h2_pal_display.h"
#include <stdint.h>

/* The board adapter observes completed RGB565 transfers in wire byte order.
 * Registration and removal happen on the sole display owner task. */
typedef void (*h2_display_esp_capture_fn)(
    void *user, const h2_display_rect_t *rect, const uint16_t *pixels);
typedef struct h2_display_esp_capture {
    int width;
    int height;
    const char *source;
    void (*set_capture)(h2_display_esp_capture_fn capture, void *user);
} h2_display_esp_capture_t;

/* Implemented by the final board entry, outside the portable test App. */
const h2_display_esp_capture_t *h2_display_esp_capture_fixture(void);

#endif
