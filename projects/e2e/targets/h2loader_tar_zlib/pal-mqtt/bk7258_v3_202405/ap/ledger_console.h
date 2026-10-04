#ifndef H2_MQTT_BK_LEDGER_CONSOLE_H
#define H2_MQTT_BK_LEDGER_CONSOLE_H

#include "h2/pal/hal/h2_pal_uart_io_stream.h"
#include <string.h>

/* Target policy: one bounded atomic write owns each complete ASCII record.
 * This holds the existing physical UART/RX owner across both resource snapshots.
 * Restore releases it before management configures a fresh, empty RX ring. */
typedef struct h2_mqtt_bk_ledger_console {
    const h2_pal_uart_io_stream_api_t *uart;
    uint32_t timeout_ms;
    int owned;
    int writable;
    int error;
} h2_mqtt_bk_ledger_console_t;

static inline int h2_mqtt_bk_console_error(h2_mqtt_bk_ledger_console_t *console, int rc) {
    if (rc != H2_PAL_OK && console->error == H2_PAL_OK) console->error = rc;
    return rc;
}

static inline int h2_mqtt_bk_console_begin(h2_mqtt_bk_ledger_console_t *console,
    const h2_pal_uart_io_stream_api_t *uart, uint32_t baud_rate, uint32_t timeout_ms) {
    console->uart = uart;
    console->timeout_ms = timeout_ms;
    if (timeout_ms == 0u || baud_rate == 0u)
        return h2_mqtt_bk_console_error(console, H2_PAL_ERR_INVALID_ARG);
    const h2_pal_uart_io_stream_config_t config = {
        .baud_rate = baud_rate, .data_bits = 8u, .stop_bits = 1u,
        .parity = H2_PAL_UART_PARITY_NONE, .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
        .rx_buffer_size = 8192u, .tx_buffer_size = 2048u,
    };
    int rc = h2_pal_uart_io_stream_configure(uart, &config);
    if (rc == H2_PAL_OK) console->owned = console->writable = 1;
    return h2_mqtt_bk_console_error(console, rc);
}

static inline int h2_mqtt_bk_console_record(h2_mqtt_bk_ledger_console_t *console,
    const char *message) {
    if (message == NULL || !console->writable)
        return h2_mqtt_bk_console_error(console, H2_PAL_ERR_INVALID_STATE);
    size_t length = strlen(message);
    char record[1024];
    if (length == 0u || length > sizeof(record) - 2u ||
        strchr(message, '\r') != NULL || strchr(message, '\n') != NULL)
        return h2_mqtt_bk_console_error(console, H2_PAL_ERR_INVALID_ARG);
    memcpy(record, message, length);
    record[length++] = '\r';
    record[length++] = '\n';
    size_t written = 0u;
    int rc = h2_pal_uart_io_stream_write(console->uart, record, length, &written,
        console->timeout_ms);
    /* Do not retry a partially emitted record or report its prefix as success. */
    if (rc == H2_PAL_OK && written != length) rc = H2_PAL_ERR_IO;
    return h2_mqtt_bk_console_error(console, rc);
}

static inline int h2_mqtt_bk_console_restore(h2_mqtt_bk_ledger_console_t *console,
    int (*drain)(void *), void (*release)(void), int (*restart)(void *), void *user) {
    if (console->owned) {
        (void)h2_mqtt_bk_console_error(console, drain(user));
        release();
    }
    console->owned = console->writable = 0;
    int rc = restart(user);
    /* A successful management restart configures the same real UART provider.
     * Subsequent READY uses its shared atomic TX mutex, without taking RX back. */
    if (rc == H2_PAL_OK) console->writable = 1;
    return h2_mqtt_bk_console_error(console, rc);
}

static inline int h2_mqtt_bk_console_can_confirm(const h2_mqtt_bk_ledger_console_t *console,
    int suite_rc) {
    return suite_rc == H2_PAL_OK && console->error == H2_PAL_OK &&
        !console->owned && console->writable;
}

#endif
