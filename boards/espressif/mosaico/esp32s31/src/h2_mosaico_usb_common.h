#ifndef H2_MOSAICO_USB_COMMON_H
#define H2_MOSAICO_USB_COMMON_H
#include "h2_pal.h"
int h2_mosaico_usb_common_init(void);
h2_pal_result_t h2_mosaico_usb_read(void*, void*, size_t, size_t*, uint32_t);
h2_pal_result_t h2_mosaico_usb_write(void*, const void*, size_t, size_t*,
                                     uint32_t);
h2_pal_result_t h2_mosaico_usb_flush(void*);
/* Best-effort diagnostics: copy into a bounded queue without waiting for USB.
 * Full queues may drop a write; framed command results use write().
 */
void h2_mosaico_usb_console_output(const void* data, size_t len);
#endif
