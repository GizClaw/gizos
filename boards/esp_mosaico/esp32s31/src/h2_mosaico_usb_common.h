#ifndef H2_MOSAICO_USB_COMMON_H
#define H2_MOSAICO_USB_COMMON_H
#include "h2_pal.h"
int h2_mosaico_usb_common_init(void);
h2_pal_result_t h2_mosaico_usb_read(void*, void*, size_t, size_t*, uint32_t);
h2_pal_result_t h2_mosaico_usb_write(void*, const void*, size_t, size_t*,
                                     uint32_t);
h2_pal_result_t h2_mosaico_usb_flush(void*);
#endif
