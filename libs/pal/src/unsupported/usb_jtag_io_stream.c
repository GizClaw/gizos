#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_usb_jtag_io_stream_read(void *p0, void *p1, size_t p2, size_t *p3, uint32_t p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_usb_jtag_io_stream_write(void *p0, const void *p1, size_t p2, size_t *p3, uint32_t p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_usb_jtag_io_stream_flush(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_usb_jtag_io_stream_vtable_t unsupported_usb_jtag_io_stream_vtable = {
    .read = unsupported_usb_jtag_io_stream_read,
    .write = unsupported_usb_jtag_io_stream_write,
    .flush = unsupported_usb_jtag_io_stream_flush,
};
static const h2_pal_usb_jtag_io_stream_api_t unsupported_usb_jtag_io_stream_api = { .user = NULL, .vtable = &unsupported_usb_jtag_io_stream_vtable };
const h2_pal_usb_jtag_io_stream_api_t *h2_pal_unsupported_usb_jtag_io_stream_api(void) { return &unsupported_usb_jtag_io_stream_api; }
