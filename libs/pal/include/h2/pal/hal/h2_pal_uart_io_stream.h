#ifndef H2_PAL_UART_IO_STREAM_H
#define H2_PAL_UART_IO_STREAM_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_uart_parity {
    H2_PAL_UART_PARITY_NONE = 0,
    H2_PAL_UART_PARITY_EVEN = 1,
    H2_PAL_UART_PARITY_ODD = 2,
} h2_pal_uart_parity_t;

typedef enum h2_pal_uart_flow_control {
    H2_PAL_UART_FLOW_CONTROL_NONE = 0,
    H2_PAL_UART_FLOW_CONTROL_RTS = 1u << 0,
    H2_PAL_UART_FLOW_CONTROL_CTS = 1u << 1,
    H2_PAL_UART_FLOW_CONTROL_RTS_CTS =
        H2_PAL_UART_FLOW_CONTROL_RTS | H2_PAL_UART_FLOW_CONTROL_CTS,
} h2_pal_uart_flow_control_t;

typedef struct h2_pal_uart_io_stream_config {
    uint32_t baud_rate;
    uint8_t data_bits;
    uint8_t stop_bits;
    h2_pal_uart_parity_t parity;
    h2_pal_uart_flow_control_t flow_control;
    size_t rx_buffer_size;
    size_t tx_buffer_size;
} h2_pal_uart_io_stream_config_t;

typedef struct h2_pal_uart_io_stream_vtable {
    h2_pal_result_t (*configure)(
        void *user,
        const h2_pal_uart_io_stream_config_t *config);
    h2_pal_result_t (*read)(
        void *user,
        void *buffer,
        size_t len,
        size_t *out_read,
        uint32_t timeout_ms);
    h2_pal_result_t (*write)(
        void *user,
        const void *buffer,
        size_t len,
        size_t *out_written,
        uint32_t timeout_ms);
    h2_pal_result_t (*flush)(void *user);
} h2_pal_uart_io_stream_vtable_t;

typedef struct h2_pal_uart_io_stream_api {
    void *user;
    const h2_pal_uart_io_stream_vtable_t *vtable;
} h2_pal_uart_io_stream_api_t;

static inline h2_pal_result_t h2_pal_uart_io_stream_configure(
    const h2_pal_uart_io_stream_api_t *api,
    const h2_pal_uart_io_stream_config_t *config) {
    if (api == NULL || config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api->vtable == NULL || api->vtable->configure == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->configure(api->user, config);
}

static inline h2_pal_result_t h2_pal_uart_io_stream_read(
    const h2_pal_uart_io_stream_api_t *api,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms) {
    if (out_read != NULL) {
        *out_read = 0u;
    }
    if (api == NULL || buffer == NULL || out_read == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api->vtable == NULL || api->vtable->read == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->read(api->user, buffer, len, out_read, timeout_ms);
}

static inline h2_pal_result_t h2_pal_uart_io_stream_write(
    const h2_pal_uart_io_stream_api_t *api,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms) {
    if (out_written != NULL) {
        *out_written = 0u;
    }
    if (api == NULL || buffer == NULL || out_written == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api->vtable == NULL || api->vtable->write == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->write(api->user, buffer, len, out_written, timeout_ms);
}

static inline h2_pal_result_t h2_pal_uart_io_stream_flush(const h2_pal_uart_io_stream_api_t *api) {
    if (api == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api->vtable == NULL || api->vtable->flush == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->flush(api->user);
}

#ifdef __cplusplus
}
#endif

#endif
