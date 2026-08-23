#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_uart_io_stream_configure(void *p0, const h2_pal_uart_io_stream_config_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_uart_io_stream_read(void *p0, void *p1, size_t p2, size_t *p3, uint32_t p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_uart_io_stream_write(void *p0, const void *p1, size_t p2, size_t *p3, uint32_t p4) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    (void)p4;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_uart_io_stream_flush(void *p0) {
    (void)p0;
    return H2_PAL_ERR_UNSUPPORTED;
}

static const h2_pal_uart_io_stream_vtable_t unsupported_uart_io_stream_vtable = {
    .configure = unsupported_uart_io_stream_configure,
    .read = unsupported_uart_io_stream_read,
    .write = unsupported_uart_io_stream_write,
    .flush = unsupported_uart_io_stream_flush,
};
static const h2_pal_uart_io_stream_api_t unsupported_uart_io_stream_api = { .user = NULL, .vtable = &unsupported_uart_io_stream_vtable };
const h2_pal_uart_io_stream_api_t *h2_pal_unsupported_uart_io_stream_api(void) { return &unsupported_uart_io_stream_api; }
