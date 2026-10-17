#ifndef H2_DISPLAY_ESP_CAPTURE_H
#define H2_DISPLAY_ESP_CAPTURE_H

#include "h2/pal/hal/h2_pal_display.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Observe completed, packed RGB565 transfers in wire byte order.
 * @param user Borrowed caller context.
 * @param rect Borrowed completed tile bounds; valid only during this callback.
 * @param pixels Borrowed width*height packed words; valid only for this call.
 * The callback must not reenter Display. The sole display owner registers and
 * removes it while other display callers are quiesced.
 */
typedef void (*h2_display_esp_capture_fn)(
    void *user, const h2_display_rect_t *rect, const uint16_t *pixels);
typedef struct h2_display_esp_capture {
    int width;
    int height;
    const char *source;
    void (*set_capture)(h2_display_esp_capture_fn capture, void *user);
} h2_display_esp_capture_t;

/**
 * @brief Borrow the final ESP entry's image-lifetime capture fixture.
 * @return Non-NULL fixture with the actual panel dimensions and completed-DMA
 * observer registration, supplied by the board entry outside the portable App.
 * The caller must unregister the observer before releasing its context.
 */
const h2_display_esp_capture_t *h2_display_esp_capture_fixture(void);

#ifdef __cplusplus
}
#endif

#endif
