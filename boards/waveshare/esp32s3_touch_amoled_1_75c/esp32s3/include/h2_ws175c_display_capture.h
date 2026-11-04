#ifndef H2_WS175C_DISPLAY_CAPTURE_H
#define H2_WS175C_DISPLAY_CAPTURE_H

#include "h2/pal/hal/h2_pal_display.h"

typedef void (*h2_ws175c_transfer_capture_fn)(void* user,
                                              const h2_display_rect_t* rect,
                                              const uint16_t* panel_pixels);

/* Diagnostic observer runs only after the actual SPI DMA transfer completes.
 * Pixels use panel byte order and remain borrowed until the callback returns.
 */
h2_display_info_t h2_ws175c_display_capture_configure(
    h2_ws175c_transfer_capture_fn capture, void* user);

#endif
