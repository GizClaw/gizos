#ifndef H2_MOSAICO_DISPLAY_CAPTURE_H
#define H2_MOSAICO_DISPLAY_CAPTURE_H
#include "h2/pal/hal/h2_pal_display.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Optional diagnostic tap of completed SPI DMA transfers, NOT panel readback.
 * Packed RGB565 words use panel byte order. Callback must return promptly,
 * cannot reenter Display, and borrows rect/pixels only for this call.
 * Configure/unregister only after quiescing display callers. */
typedef void (*h2_mosaico_transfer_capture_fn)(void *user,
                                              const h2_display_rect_t *rect,
                                              const uint16_t *pixels);
void h2_mosaico_display_set_transfer_capture(
    h2_mosaico_transfer_capture_fn capture, void *user);
#ifdef __cplusplus
}
#endif
#endif
