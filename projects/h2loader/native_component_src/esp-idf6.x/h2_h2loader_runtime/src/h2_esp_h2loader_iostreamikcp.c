#include "h2_esp_h2loader_iostreamikcp.h"
#include "h2_esp_h2loader_iostreamikcp_internal.h"

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#define H2_LOADER_SERIAL_RX_BUFFER 8192u
#define H2_LOADER_SERIAL_TX_BUFFER 2048u
#define H2_LOADER_CONSOLE_UART ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)
#define H2_LOADER_UART_BAUD_RATE 230400u
#define H2_LOADER_TRANSPORT_DEFAULT_WRITE_TIMEOUT_MS 5000u
#define H2_LOADER_TRANSPORT_MAX_FLUSH_TIMEOUT_MS 120000u
#define H2_LOADER_TRANSPORT_MAX_WAITSND 64u
#define H2_LOADER_TRANSPORT_POLL_INTERVAL_MS 10u
#define H2_LOADER_TRANSPORT_PHYSICAL_READ_SIZE 512u
#define H2_LOADER_TRANSPORT_SEGMENT_TIMEOUT_MS 60u
#define H2_LOADER_TRANSPORT_RECEIVE_WINDOW 20u

typedef struct h2_esp_h2loader_app_iostreamikcp {
    h2_esp_h2loader_command_transport_t transport;
    h2_command_io_api_t io;
} h2_esp_h2loader_app_iostreamikcp_t;

static h2_esp_h2loader_app_iostreamikcp_t s_app_transport;
static int s_app_transport_started;
static int s_console_initialized;

static uint32_t transport_now_ms(void *user) {
    (void)user;
    return (uint32_t)((uint64_t)xTaskGetTickCount() * (uint64_t)portTICK_PERIOD_MS);
}

h2_pal_result_t h2_esp_h2loader_console_init(void) {
    if (s_console_initialized) {
        return H2_PAL_OK;
    }
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t config = {
        .rx_buffer_size = H2_LOADER_SERIAL_RX_BUFFER,
        .tx_buffer_size = H2_LOADER_SERIAL_TX_BUFFER,
    };
    esp_err_t err = ESP_OK;
    if (usb_serial_jtag_is_driver_installed()) {
        err = usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000u));
        if (err == ESP_OK) {
            err = usb_serial_jtag_driver_uninstall();
        }
    }
    if (err == ESP_OK) {
        err = usb_serial_jtag_driver_install(&config);
    }
    if (err == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();
    }
#else
    esp_err_t err = uart_driver_install(
        H2_LOADER_CONSOLE_UART,
        H2_LOADER_SERIAL_RX_BUFFER,
        H2_LOADER_SERIAL_TX_BUFFER,
        0,
        NULL,
        0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        /* Route stdio through the UART driver's TX lock so a console character
         * cannot be enqueued inside one IO Stream iKCP frame write. */
        uart_vfs_dev_use_driver(H2_LOADER_CONSOLE_UART);
        err = uart_set_baudrate(H2_LOADER_CONSOLE_UART, H2_LOADER_UART_BAUD_RATE);
    }
    if (err == ESP_OK) {
        (void)uart_flush_input(H2_LOADER_CONSOLE_UART);
    }
#endif
    if (err == ESP_OK) {
        s_console_initialized = 1;
        return H2_PAL_OK;
    }
    return err == ESP_ERR_TIMEOUT ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_IO;
}

static void write_le32(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

static h2_pal_result_t transport_send_control(
    h2_esp_h2loader_command_transport_t *transport,
    uint8_t flags,
    uint32_t conv) {
    uint8_t payload[H2_IOSTREAMIKCP_SESSION_CONTROL_PAYLOAD_LEN];
    uint8_t encoded[H2_IOSTREAMIKCP_FRAME_HEADER_LEN + H2_IOSTREAMIKCP_SESSION_CONTROL_PAYLOAD_LEN];
    size_t encoded_len = 0u;
    size_t written = 0u;
    write_le32(payload, conv);
    h2_iostreamikcp_frame_t frame = {
        .flags = flags,
        .conv = conv,
        .payload = payload,
        .payload_len = sizeof(payload),
    };
    h2_pal_result_t rc = h2_iostreamikcp_frame_encode(&frame, encoded, sizeof(encoded), &encoded_len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = transport->physical_io.write(
        transport->physical_io.user, encoded, encoded_len, &written, 1000u);
    if (rc != H2_PAL_OK || written != encoded_len) {
        return rc != H2_PAL_OK ? rc : H2_PAL_ERR_IO;
    }
    return transport->physical_io.flush != NULL ?
        transport->physical_io.flush(transport->physical_io.user) : H2_PAL_OK;
}

static h2_pal_result_t transport_on_frame(void *user, const h2_iostreamikcp_frame_t *frame) {
    h2_esp_h2loader_command_transport_t *transport = user;
    if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN) {
        if (frame->conv == transport->conv && transport->stream != NULL) {
            return transport_send_control(
                transport, H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK, frame->conv);
        }
        transport->pending_conv = frame->conv;
        transport->replacement_pending = transport->stream != NULL;
        return H2_PAL_OK;
    }
    if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_DATA &&
        transport->stream != NULL && frame->conv == transport->conv) {
        return h2_iostreamikcp_input_frame(transport->stream, frame);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t transport_poll_physical(
    h2_esp_h2loader_command_transport_t *transport,
    uint32_t timeout_ms) {
    uint8_t buffer[H2_LOADER_TRANSPORT_PHYSICAL_READ_SIZE];
    size_t count = 0u;
    uint32_t read_timeout_ms = timeout_ms;
    if (transport->stream != NULL &&
        read_timeout_ms > H2_LOADER_TRANSPORT_POLL_INTERVAL_MS) {
        read_timeout_ms = H2_LOADER_TRANSPORT_POLL_INTERVAL_MS;
    }
    h2_pal_result_t read_rc = transport->physical_io.read(
        transport->physical_io.user, buffer, sizeof(buffer), &count, read_timeout_ms);
    if (read_rc != H2_PAL_OK && read_rc != H2_PAL_ERR_TIMEOUT &&
        read_rc != H2_PAL_ERR_WOULD_BLOCK) {
        return read_rc;
    }
    h2_pal_result_t rc = H2_PAL_OK;
    if (count > 0u) {
        rc = h2_iostreamikcp_filter_input(
            &transport->filter, buffer, count, transport_on_frame, transport);
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (transport->stream != NULL) {
        rc = h2_iostreamikcp_update(transport->stream, transport_now_ms(NULL));
    }
    return rc != H2_PAL_OK ? rc : read_rc;
}

static h2_pal_result_t command_read(
    void *user,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms) {
    h2_esp_h2loader_command_transport_t *transport = user;
    if (transport == NULL || buffer == NULL || out_read == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (transport->stream == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    uint32_t started = transport_now_ms(NULL);
    *out_read = 0u;
    for (;;) {
        if (transport->replacement_pending) {
            return H2_PAL_ERR_CLOSED;
        }
        h2_pal_result_t rc = h2_iostreamikcp_read(transport->stream, buffer, len, out_read);
        if (rc != H2_PAL_ERR_WOULD_BLOCK) {
            return rc;
        }
        uint32_t elapsed = transport_now_ms(NULL) - started;
        if (elapsed >= timeout_ms) {
            return H2_PAL_ERR_TIMEOUT;
        }
        rc = transport_poll_physical(transport, timeout_ms - elapsed);
        if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_WOULD_BLOCK) {
            return rc;
        }
    }
}

static h2_pal_result_t command_write(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms) {
    h2_esp_h2loader_command_transport_t *transport = user;
    if (transport == NULL || buffer == NULL || out_written == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_written = 0u;
    if (transport->stream == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (transport->replacement_pending) {
        return H2_PAL_ERR_CLOSED;
    }
    transport->write_timeout_ms = timeout_ms == 0u ?
        H2_LOADER_TRANSPORT_DEFAULT_WRITE_TIMEOUT_MS : timeout_ms;
    uint32_t started = transport_now_ms(NULL);
    for (;;) {
        h2_iostreamikcp_stats_t stats;
        h2_pal_result_t stats_rc = h2_iostreamikcp_get_stats(transport->stream, &stats);
        if (stats_rc != H2_PAL_OK || stats.waitsnd < H2_LOADER_TRANSPORT_MAX_WAITSND) {
            if (stats_rc != H2_PAL_OK) {
                return stats_rc;
            }
            break;
        }
        if (transport->replacement_pending) {
            return H2_PAL_ERR_CLOSED;
        }
        uint32_t elapsed = transport_now_ms(NULL) - started;
        if (elapsed >= transport->write_timeout_ms) {
            return H2_PAL_ERR_TIMEOUT;
        }
        uint32_t remaining_ms = transport->write_timeout_ms - elapsed;
        uint32_t poll_ms = remaining_ms < H2_LOADER_TRANSPORT_POLL_INTERVAL_MS ?
            remaining_ms : H2_LOADER_TRANSPORT_POLL_INTERVAL_MS;
        h2_pal_result_t pump_rc = transport_poll_physical(transport, poll_ms);
        if (pump_rc != H2_PAL_OK && pump_rc != H2_PAL_ERR_TIMEOUT &&
            pump_rc != H2_PAL_ERR_WOULD_BLOCK) {
            return pump_rc;
        }
    }
    h2_pal_result_t rc = h2_iostreamikcp_write(transport->stream, buffer, len);
    *out_written = rc == H2_PAL_OK ? len : 0u;
    if (rc == H2_PAL_OK) {
        h2_pal_result_t poll_rc = transport_poll_physical(transport, 0u);
        if (poll_rc != H2_PAL_OK && poll_rc != H2_PAL_ERR_TIMEOUT &&
            poll_rc != H2_PAL_ERR_WOULD_BLOCK) {
            return poll_rc;
        }
    }
    return rc;
}

static h2_pal_result_t command_flush(void *user) {
    h2_esp_h2loader_command_transport_t *transport = user;
    if (transport == NULL || transport->stream == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if (transport->replacement_pending) {
        return H2_PAL_ERR_CLOSED;
    }
    h2_iostreamikcp_stats_t stats = { 0 };
    h2_pal_result_t rc = h2_iostreamikcp_flush(transport->stream);
    if (rc == H2_PAL_OK) {
        rc = h2_iostreamikcp_get_stats(transport->stream, &stats);
    }
    uint64_t estimated_ms = (uint64_t)stats.waitsnd * H2_LOADER_TRANSPORT_SEGMENT_TIMEOUT_MS;
    uint32_t timeout_ms = transport->write_timeout_ms;
    if (estimated_ms > timeout_ms) {
        timeout_ms = estimated_ms > H2_LOADER_TRANSPORT_MAX_FLUSH_TIMEOUT_MS ?
            H2_LOADER_TRANSPORT_MAX_FLUSH_TIMEOUT_MS : (uint32_t)estimated_ms;
    }
    uint32_t started = transport_now_ms(NULL);
    while (rc == H2_PAL_OK) {
        rc = h2_iostreamikcp_get_stats(transport->stream, &stats);
        if (rc != H2_PAL_OK || stats.waitsnd == 0u) {
            break;
        }
        if (transport->replacement_pending) {
            return H2_PAL_ERR_CLOSED;
        }
        uint32_t elapsed = transport_now_ms(NULL) - started;
        if (elapsed >= timeout_ms) {
            return H2_PAL_ERR_TIMEOUT;
        }
        uint32_t remaining_ms = timeout_ms - elapsed;
        uint32_t poll_ms = remaining_ms < H2_LOADER_TRANSPORT_POLL_INTERVAL_MS ?
            remaining_ms : H2_LOADER_TRANSPORT_POLL_INTERVAL_MS;
        rc = transport_poll_physical(transport, poll_ms);
        if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_WOULD_BLOCK) {
            rc = h2_iostreamikcp_update(transport->stream, transport_now_ms(NULL));
        }
    }
    return rc;
}

static const h2_command_io_vtable_t s_command_io_vtable = {
    .read = command_read,
    .write = command_write,
    .flush = command_flush,
};

h2_pal_result_t h2_esp_h2loader_command_transport_init(
    h2_esp_h2loader_command_transport_t *transport,
    const h2_pal_mem_api_t *allocator) {
    if (transport == NULL || allocator == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(transport, 0, sizeof(*transport));
    transport->allocator = allocator;
    transport->write_timeout_ms = H2_LOADER_TRANSPORT_DEFAULT_WRITE_TIMEOUT_MS;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    transport->physical_io = h2_iostreamikcp_io_from_usb_jtag(
        h2_esp_platform_usb_jtag_io_stream_api());
#else
    transport->physical_io = h2_iostreamikcp_io_from_uart(
        h2_esp_platform_uart_io_stream_api());
#endif
    transport->receive_window = H2_LOADER_TRANSPORT_RECEIVE_WINDOW;
    h2_iostreamikcp_filter_init(&transport->filter);
    return H2_PAL_OK;
}

void h2_esp_h2loader_command_transport_deinit(
    h2_esp_h2loader_command_transport_t *transport) {
    if (transport != NULL) {
        h2_iostreamikcp_close(transport->stream);
        transport->stream = NULL;
    }
}

h2_command_io_api_t h2_esp_h2loader_command_transport_io(
    h2_esp_h2loader_command_transport_t *transport) {
    h2_command_io_api_t io = {
        .user = transport,
        .vtable = &s_command_io_vtable,
    };
    return io;
}

h2_pal_result_t h2_esp_h2loader_command_transport_poll_session(
    h2_esp_h2loader_command_transport_t *transport,
    uint32_t timeout_ms) {
    if (transport == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return transport_poll_physical(transport, timeout_ms);
}

h2_pal_result_t h2_esp_h2loader_command_transport_activate_pending(
    h2_esp_h2loader_command_transport_t *transport) {
    if (transport == NULL || transport->pending_conv == 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    uint32_t conv = transport->pending_conv;
    h2_iostreamikcp_close(transport->stream);
    transport->stream = NULL;
    h2_iostreamikcp_config_t config = {
        .io = transport->physical_io,
        .allocator = transport->allocator,
        .now_ms = transport_now_ms,
        .conv = conv,
        .mtu = H2_IOSTREAMIKCP_DEFAULT_MTU,
        .rx_buffer_size = 4096u,
        .receive_window = transport->receive_window,
        .write_timeout_ms = 1000u,
    };
    h2_pal_result_t rc = h2_iostreamikcp_open(&config, &transport->stream);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    transport->conv = conv;
    transport->pending_conv = 0u;
    transport->replacement_pending = 0;
    rc = transport_send_control(
        transport, H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK, conv);
    if (rc != H2_PAL_OK) {
        h2_iostreamikcp_close(transport->stream);
        transport->stream = NULL;
        transport->conv = 0u;
    }
    return rc;
}

int h2_esp_h2loader_command_transport_has_session(
    const h2_esp_h2loader_command_transport_t *transport) {
    return transport != NULL && transport->stream != NULL;
}

int h2_esp_h2loader_command_transport_replacement_pending(
    const h2_esp_h2loader_command_transport_t *transport) {
    return transport != NULL && transport->replacement_pending;
}

static int app_read_byte(void *user, uint32_t timeout_ms) {
    h2_esp_h2loader_app_iostreamikcp_t *state = user;
    uint8_t value = 0u;
    size_t count = 0u;

    if (state == NULL) {
        return EOF;
    }
    for (;;) {
        if (state->transport.pending_conv != 0u) {
            h2_pal_result_t rc = h2_esp_h2loader_command_transport_activate_pending(
                &state->transport);
            return rc == H2_PAL_OK ? H2_LOADER_APP_CLIENT_SESSION_RESET : EOF;
        }
        if (!h2_esp_h2loader_command_transport_has_session(&state->transport)) {
            h2_pal_result_t rc = h2_esp_h2loader_command_transport_poll_session(
                &state->transport, timeout_ms);
            if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
                rc != H2_PAL_ERR_WOULD_BLOCK) {
                return EOF;
            }
            if (state->transport.pending_conv == 0u) {
                return EOF;
            }
            continue;
        }
        h2_pal_result_t rc = state->io.vtable->read(
            state->io.user, &value, sizeof(value), &count, timeout_ms);
        if (rc == H2_PAL_OK && count == sizeof(value)) {
            return value;
        }
        if (rc == H2_PAL_ERR_CLOSED &&
            h2_esp_h2loader_command_transport_replacement_pending(&state->transport)) {
            continue;
        }
        return EOF;
    }
}

static int app_write(void *user, const char *data, size_t len) {
    h2_esp_h2loader_app_iostreamikcp_t *state = user;
    size_t offset = 0u;

    if (state == NULL || (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    while (offset < len) {
        size_t written = 0u;
        h2_pal_result_t rc = state->io.vtable->write(
            state->io.user, data + offset, len - offset, &written, 5000u);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (written == 0u || written > len - offset) {
            return H2_PAL_ERR_IO;
        }
        offset += written;
    }
    return state->io.vtable->flush(state->io.user);
}

int h2_esp_h2loader_app_iostreamikcp_start(
    h2_loader_app_client_t *client,
    const h2_pal_task_api_t *task,
    const h2_pal_mem_api_t *allocator,
    size_t stack_size) {
    if (client == NULL || task == NULL || allocator == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (s_app_transport_started) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_esp_h2loader_app_iostreamikcp_t *state = &s_app_transport;
    memset(state, 0, sizeof(*state));
    h2_pal_result_t rc = h2_esp_h2loader_console_init();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_esp_h2loader_command_transport_init(&state->transport, allocator);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    state->io = h2_esp_h2loader_command_transport_io(&state->transport);
    h2_loader_app_client_return_console_config_t config = {
        .client = client,
        .task = task,
        .read_user = state,
        .read_byte = app_read_byte,
        .write_user = state,
        .write = app_write,
        .stack_size = stack_size,
    };
    rc = h2_loader_app_client_start_return_console(&config);
    if (rc != H2_PAL_OK) {
        h2_esp_h2loader_command_transport_deinit(&state->transport);
        memset(state, 0, sizeof(*state));
    } else {
        s_app_transport_started = 1;
    }
    return rc;
}
