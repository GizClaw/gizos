#ifndef H2_ZERO_BK_DISPLAY_CAPTURE_H
#define H2_ZERO_BK_DISPLAY_CAPTURE_H
#include "h2/pal/hal/h2_pal_display.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Synchronous callback after successful SDK SPI transfer and FIFO drain.
 * RGB565 wire bytes (MSB first) and rect are borrowed for this call only.
 * Evidence covers transfers, not optical pixels. The serialized Display owner
 * must install/remove callbacks while idle, and keep user storage alive until
 * removal. NULL disables capture. Returns panel geometry without opening it. */
typedef void (*h2_zero_bk_display_capture_fn)(void* user,
                                              const h2_display_rect_t* rect,
                                              const uint8_t* bytes, size_t len);
h2_display_info_t h2_zero_bk_display_set_transfer_capture(
    h2_zero_bk_display_capture_fn capture, void* user);
/** Last successful GPIO/PWM percentage, not measured luminance.
 * Borrow this observation from the serialized Display owner while open. */
uint32_t h2_zero_bk_display_backlight_percent(void);
#ifdef __cplusplus
}
#endif
#endif
