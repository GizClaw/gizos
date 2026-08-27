#include "h2_h2loader_host.h"

#include "h2_h2loader_host_internal.h"
#include "h2_iostreamikcp.h"

#include <stdio.h>
#include <string.h>

#define H2_H2LOADER_HOST_SERIAL_POLL_MS 10u
#define H2_H2LOADER_HOST_SERIAL_SESSION_RETRY_MS 200u
#define H2_H2LOADER_HOST_SERIAL_RESPONSE_ACK_GRACE_MS 100u
#define H2_H2LOADER_HOST_SERIAL_RECEIVE_WINDOW 64u
#define H2_H2LOADER_HOST_SERIAL_RX_SIZE (64u * 1024u)
#define H2_H2LOADER_HOST_SERIAL_WRITE_BATCH (64u * 1024u)
#define H2_H2LOADER_HOST_SERIAL_RESPONSE_SIZE 8192u

struct h2_h2loader_host_serial_connection {
    const h2_pal_serial_host_api_t *serial;
    const h2_pal_time_api_t *time;
    const h2_pal_mem_api_t *allocator;
    h2_pal_serial_host_session_t *session;
    const h2_pal_uart_io_stream_api_t *uart;
    h2_iostreamikcp_t *stream;
    uint32_t command_timeout_ms;
    uint32_t post_command_delay_ms;
};

typedef struct session_ack_context {
    uint32_t conversation_id;
    int received;
} session_ack_context_t;

static h2_pal_result_t serial_now(
    const h2_pal_time_api_t *time,
    uint64_t *out_now) {
    return h2_pal_time_get_monotonic_ms(time, out_now);
}

static uint32_t serial_stream_now_ms(void *user) {
    h2_h2loader_host_serial_connection_t *connection = user;
    uint64_t now = 0u;
    if (connection == NULL ||
        serial_now(connection->time, &now) != H2_PAL_OK) {
        return 0u;
    }
    return (uint32_t)now;
}

static h2_pal_result_t session_frame(
    void *user,
    const h2_iostreamikcp_frame_t *frame) {
    session_ack_context_t *context = user;
    if (context == NULL || frame == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK &&
        frame->conv == context->conversation_id) {
        context->received = 1;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t uart_write_all(
    const h2_pal_uart_io_stream_api_t *uart,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    while (len > 0u) {
        size_t written = 0u;
        h2_pal_result_t rc = h2_pal_uart_io_stream_write(
            uart, data, len, &written, timeout_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (written == 0u || written > len) {
            return H2_PAL_ERR_IO;
        }
        data += written;
        len -= written;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t serial_handshake(
    h2_h2loader_host_serial_connection_t *connection,
    uint32_t conversation_id,
    uint32_t timeout_ms) {
    h2_iostreamikcp_filter_t filter;
    h2_iostreamikcp_frame_t open_frame;
    session_ack_context_t ack = {
        .conversation_id = conversation_id,
    };
    uint8_t control[H2_IOSTREAMIKCP_SESSION_CONTROL_PAYLOAD_LEN] = {
        (uint8_t)conversation_id,
        (uint8_t)(conversation_id >> 8u),
        (uint8_t)(conversation_id >> 16u),
        (uint8_t)(conversation_id >> 24u),
    };
    uint8_t encoded[
        H2_IOSTREAMIKCP_FRAME_HEADER_LEN +
        H2_IOSTREAMIKCP_SESSION_CONTROL_PAYLOAD_LEN];
    size_t encoded_len = 0u;
    uint64_t start = 0u;
    uint64_t now = 0u;
    uint64_t next_send = 0u;

    open_frame.flags = H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN;
    open_frame.conv = conversation_id;
    open_frame.payload = control;
    open_frame.payload_len = sizeof(control);
    h2_pal_result_t rc = h2_iostreamikcp_frame_encode(
        &open_frame, encoded, sizeof(encoded), &encoded_len);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    h2_iostreamikcp_filter_init(&filter);
    rc = serial_now(connection->time, &start);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    now = start;
    next_send = start;
    while (now - start < timeout_ms) {
        if (now >= next_send) {
            rc = uart_write_all(
                connection->uart, encoded, encoded_len, timeout_ms);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            rc = h2_pal_uart_io_stream_flush(connection->uart);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            next_send = now + H2_H2LOADER_HOST_SERIAL_SESSION_RETRY_MS;
        }
        uint8_t input[512];
        size_t read = 0u;
        rc = h2_pal_uart_io_stream_read(
            connection->uart,
            input,
            sizeof(input),
            &read,
            H2_H2LOADER_HOST_SERIAL_POLL_MS);
        if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
            rc != H2_PAL_ERR_WOULD_BLOCK) {
            return rc;
        }
        if (read > 0u) {
            rc = h2_iostreamikcp_filter_input(
                &filter, input, read, session_frame, &ack);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            if (ack.received) {
                return H2_PAL_OK;
            }
        }
        rc = serial_now(connection->time, &now);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t serial_wait_ready_marker(
    h2_h2loader_host_serial_connection_t *connection,
    const char *marker,
    uint32_t timeout_ms) {
    size_t marker_len;
    size_t matched = 0u;
    uint64_t start = 0u;
    uint64_t now = 0u;
    h2_pal_result_t rc;
    if (marker == NULL) return H2_PAL_OK;
    marker_len = strlen(marker);
    if (marker_len == 0u || marker_len > 1024u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = serial_now(connection->time, &start);
    if (rc != H2_PAL_OK) return rc;
    now = start;
    while (now - start < timeout_ms) {
        uint8_t input[512];
        size_t read = 0u;
        rc = h2_pal_uart_io_stream_read(
            connection->uart,
            input,
            sizeof(input),
            &read,
            H2_H2LOADER_HOST_SERIAL_POLL_MS);
        if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
            rc != H2_PAL_ERR_WOULD_BLOCK) return rc;
        for (size_t i = 0u; i < read; ++i) {
            if (input[i] == (uint8_t)marker[matched]) {
                ++matched;
                if (matched == marker_len) return H2_PAL_OK;
            } else {
                matched = input[i] == (uint8_t)marker[0] ? 1u : 0u;
            }
        }
        rc = serial_now(connection->time, &now);
        if (rc != H2_PAL_OK) return rc;
    }
    return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t serial_pump(
    h2_h2loader_host_serial_connection_t *connection,
    uint32_t timeout_ms) {
    uint64_t now = 0u;
    h2_pal_result_t rc = h2_iostreamikcp_poll(
        connection->stream, timeout_ms);
    if (rc != H2_PAL_OK && rc != H2_PAL_ERR_TIMEOUT &&
        rc != H2_PAL_ERR_WOULD_BLOCK) {
        return rc;
    }
    rc = serial_now(connection->time, &now);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return h2_iostreamikcp_update(
        connection->stream, (uint32_t)now);
}

static h2_pal_result_t serial_wait_delivery(
    h2_h2loader_host_serial_connection_t *connection,
    uint32_t timeout_ms) {
    uint64_t start = 0u;
    uint64_t now = 0u;
    h2_pal_result_t rc = serial_now(connection->time, &start);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    now = start;
    while (now - start < timeout_ms) {
        h2_iostreamikcp_stats_t stats;
        rc = h2_iostreamikcp_get_stats(connection->stream, &stats);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        if (stats.waitsnd == 0u) {
            return H2_PAL_OK;
        }
        rc = serial_pump(
            connection, H2_H2LOADER_HOST_SERIAL_POLL_MS);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = serial_now(connection->time, &now);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_ERR_TIMEOUT;
}

static int response_has_complete_marker(
    const uint8_t *response,
    size_t response_len,
    const char *marker) {
    size_t marker_len = strlen(marker);
    if (marker_len == 0u || response_len < marker_len + 1u) {
        return 0;
    }
    for (size_t i = 0u; i + marker_len < response_len; ++i) {
        if (memcmp(&response[i], marker, marker_len) != 0) {
            continue;
        }
        for (size_t j = i + marker_len; j < response_len; ++j) {
            if (response[j] == '\n' || response[j] == '\r') {
                return 1;
            }
        }
    }
    return 0;
}

static const char *find_line_marker(const char *response, const char *marker) {
    const char *cursor = response;
    while ((cursor = strstr(cursor, marker)) != NULL) {
        if (cursor == response || cursor[-1] == '\n' || cursor[-1] == '\r') {
            return cursor;
        }
        ++cursor;
    }
    return NULL;
}

static h2_pal_result_t serial_read_until(
    h2_h2loader_host_serial_connection_t *connection,
    const char *marker_a,
    const char *marker_b,
    uint8_t *response,
    size_t response_size,
    size_t *out_len,
    uint32_t timeout_ms,
    h2_h2loader_host_command_output_fn on_output,
    void *output_user) {
    uint64_t start = 0u;
    uint64_t now = 0u;
    size_t length = 0u;
    h2_pal_result_t rc;

    *out_len = 0u;
    rc = serial_now(connection->time, &start);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    now = start;
    while (now - start < timeout_ms) {
        rc = serial_pump(
            connection, H2_H2LOADER_HOST_SERIAL_POLL_MS);
        if (rc != H2_PAL_OK) {
            *out_len = length;
            return rc;
        }
        for (;;) {
            size_t read = 0u;
            if (length == response_size) {
                *out_len = length;
                return H2_PAL_ERR_NO_SPACE;
            }
            rc = h2_iostreamikcp_read(
                connection->stream,
                &response[length],
                response_size - length,
                &read);
            if (rc == H2_PAL_ERR_WOULD_BLOCK) {
                break;
            }
            if (rc != H2_PAL_OK) {
                *out_len = length;
                return rc;
            }
            if (read == 0u) {
                break;
            }
            if (on_output != NULL) {
                rc = on_output(output_user, &response[length], read);
                if (rc != H2_PAL_OK) {
                    length += read;
                    *out_len = length;
                    return rc;
                }
            }
            length += read;
        }
        if (response_has_complete_marker(response, length, marker_a) &&
            (marker_b == NULL ||
             response_has_complete_marker(response, length, marker_b))) {
            *out_len = length;
            return h2_iostreamikcp_flush(connection->stream);
        }
        rc = serial_now(connection->time, &now);
        if (rc != H2_PAL_OK) {
            *out_len = length;
            return rc;
        }
    }
    *out_len = length;
    return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t serial_write_command(
    h2_h2loader_host_serial_connection_t *connection,
    const char *command) {
    h2_pal_result_t rc = h2_iostreamikcp_write(
        connection->stream,
        (const uint8_t *)command,
        strlen(command));
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = serial_wait_delivery(connection, connection->command_timeout_ms);
    if (rc == H2_PAL_OK && connection->post_command_delay_ms != 0u) {
        rc = h2_pal_time_sleep_ms(
            connection->time, connection->post_command_delay_ms);
    }
    return rc;
}

static int marker_result_is_ok(
    const uint8_t *response,
    size_t response_len,
    const char *marker) {
    size_t marker_len = strlen(marker);
    for (size_t i = 0u; i + marker_len + 2u <= response_len; ++i) {
        if (memcmp(&response[i], marker, marker_len) == 0 &&
            response[i + marker_len] == 'O' &&
            response[i + marker_len + 1u] == 'K') {
            return 1;
        }
    }
    return 0;
}

h2_pal_result_t h2_h2loader_host_serial_connect(
    const h2_h2loader_host_serial_connection_config_t *config,
    h2_h2loader_host_serial_connection_t **out_connection) {
    h2_h2loader_host_serial_connection_t *connection = NULL;
    uint32_t conversation_id;
    uint64_t now = 0u;

    if (out_connection != NULL) {
        *out_connection = NULL;
    }
    if (config == NULL || out_connection == NULL ||
        config->serial == NULL || config->time == NULL ||
        config->allocator == NULL || config->port_id == NULL ||
        config->port_id[0] == '\0') {
        return H2_PAL_ERR_INVALID_ARG;
    }
    connection = h2_pal_mem_alloc(
        config->allocator, sizeof(*connection));
    if (connection == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(connection, 0, sizeof(*connection));
    connection->serial = config->serial;
    connection->time = config->time;
    connection->allocator = config->allocator;
    connection->command_timeout_ms = config->command_timeout_ms == 0u
        ? H2_H2LOADER_HOST_DEFAULT_COMMAND_TIMEOUT_MS
        : config->command_timeout_ms;
    connection->post_command_delay_ms = config->post_command_delay_ms;
    const h2_pal_uart_io_stream_config_t uart_config = {
        .baud_rate = H2_H2LOADER_HOST_RELIABLE_SERIAL_BAUD,
        .data_bits = 8u,
        .stop_bits = 1u,
        .parity = H2_PAL_UART_PARITY_NONE,
        .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE,
        .rx_buffer_size = H2_H2LOADER_HOST_SERIAL_RX_SIZE,
        .tx_buffer_size = 8192u,
    };
    h2_pal_result_t rc = h2_pal_serial_host_open(
        config->serial,
        config->port_id,
        &uart_config,
        &connection->session);
    if (rc != H2_PAL_OK) {
        goto fail;
    }
    rc = h2_pal_serial_host_set_control_lines(
        config->serial,
        connection->session,
        H2_PAL_SERIAL_HOST_CONTROL_DTR |
            H2_PAL_SERIAL_HOST_CONTROL_RTS,
        0u);
    if (rc == H2_PAL_ERR_UNSUPPORTED) {
        rc = H2_PAL_OK;
    } else if (rc != H2_PAL_OK) {
        goto fail;
    }
    rc = h2_pal_serial_host_session_stream(
        config->serial, connection->session, &connection->uart);
    if (rc != H2_PAL_OK) {
        goto fail;
    }
    rc = serial_wait_ready_marker(
        connection,
        config->ready_marker,
        config->handshake_timeout_ms == 0u
            ? H2_H2LOADER_HOST_DEFAULT_COMMAND_TIMEOUT_MS
            : config->handshake_timeout_ms);
    if (rc != H2_PAL_OK) {
        goto fail;
    }
    conversation_id = config->conversation_id;
    if (conversation_id == 0u) {
        rc = serial_now(config->time, &now);
        if (rc != H2_PAL_OK) {
            goto fail;
        }
        conversation_id = (uint32_t)(now ^ (now >> 32u) ^
            (uintptr_t)connection);
        if (conversation_id == 0u) {
            conversation_id = 1u;
        }
    }
    rc = serial_handshake(
        connection,
        conversation_id,
        config->handshake_timeout_ms == 0u
            ? H2_H2LOADER_HOST_DEFAULT_COMMAND_TIMEOUT_MS
            : config->handshake_timeout_ms);
    if (rc != H2_PAL_OK) {
        goto fail;
    }
    const h2_iostreamikcp_config_t stream_config = {
        .io = h2_iostreamikcp_io_from_uart(connection->uart),
        .allocator = config->allocator,
        .now_ms = serial_stream_now_ms,
        .time_user = connection,
        .conv = conversation_id,
        .mtu = H2_IOSTREAMIKCP_DEFAULT_MTU,
        .rx_buffer_size = H2_H2LOADER_HOST_SERIAL_RX_SIZE,
        .receive_window = H2_H2LOADER_HOST_SERIAL_RECEIVE_WINDOW,
        .write_timeout_ms = connection->command_timeout_ms,
        .on_log = config->on_log,
        .log_user = config->log_user,
    };
    rc = h2_iostreamikcp_open(&stream_config, &connection->stream);
    if (rc != H2_PAL_OK) {
        goto fail;
    }
    *out_connection = connection;
    return H2_PAL_OK;

fail:
    if (connection->stream != NULL) {
        h2_iostreamikcp_close(connection->stream);
    }
    if (connection->session != NULL) {
        (void)h2_pal_serial_host_close(
            config->serial, &connection->session);
    }
    h2_pal_mem_free(config->allocator, connection);
    return rc;
}

h2_pal_result_t h2_h2loader_host_serial_monitor_logs(
    h2_h2loader_host_serial_connection_t *connection,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user) {
    if (connection == NULL || is_cancelled == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    while (!is_cancelled(cancel_user)) {
        h2_pal_result_t rc = serial_pump(
            connection, H2_H2LOADER_HOST_SERIAL_POLL_MS);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_EXIT;
}

h2_pal_result_t h2_h2loader_host_serial_disconnect(
    h2_h2loader_host_serial_connection_t **inout_connection) {
    h2_h2loader_host_serial_connection_t *connection;
    h2_pal_result_t rc;

    if (inout_connection == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    connection = *inout_connection;
    if (connection == NULL) {
        return H2_PAL_OK;
    }
    *inout_connection = NULL;
    if (connection->stream != NULL) {
        h2_iostreamikcp_close(connection->stream);
        connection->stream = NULL;
    }
    rc = h2_pal_serial_host_close(
        connection->serial, &connection->session);
    h2_pal_mem_free(connection->allocator, connection);
    return rc;
}

h2_pal_result_t h2_h2loader_host_serial_read_status(
    h2_h2loader_host_serial_connection_t *connection,
    h2_h2loader_host_status_t *out_status) {
    uint8_t response[H2_H2LOADER_HOST_SERIAL_RESPONSE_SIZE];
    size_t response_len = 0u;
    static const char loader_marker[] = "H2_LOADER_STATUS ";

    if (out_status != NULL) {
        memset(out_status, 0, sizeof(*out_status));
    }
    if (connection == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc =
        serial_write_command(connection, "h2loader status\n");
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = serial_read_until(
        connection,
        loader_marker,
        NULL,
        response,
        sizeof(response) - 1u,
        &response_len,
        connection->command_timeout_ms,
        NULL,
        NULL);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    response[response_len] = '\0';
    const char *marker = find_line_marker(
        (const char *)response, loader_marker);
    return marker == NULL
        ? H2_PAL_ERR_FORMAT
        : h2_h2loader_host_status_parse(marker, out_status);
}

static h2_pal_result_t serial_command_write(
    void *transport,
    const char *line) {
    return serial_write_command(transport, line);
}

static h2_pal_result_t serial_command_read(
    void *transport,
    const char *marker,
    uint8_t *response,
    size_t response_size,
    size_t *out_response_len,
    h2_h2loader_host_command_output_fn on_output,
    void *output_user) {
    h2_h2loader_host_serial_connection_t *connection = transport;
    return serial_read_until(
        connection,
        marker,
        NULL,
        response,
        response_size,
        out_response_len,
        connection->command_timeout_ms + 30000u,
        on_output,
        output_user);
}

static h2_pal_result_t serial_finish_command_response(void *transport) {
    h2_h2loader_host_serial_connection_t *connection = transport;
    uint64_t start = 0u;
    uint64_t now = 0u;
    h2_pal_result_t rc;

    if (connection == NULL || connection->stream == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    /* serial_read_until() already flushed the KCP ACK after the complete
     * terminal line. Keep pumping for a bounded grace period so the peer can
     * consume that ACK before this side closes the physical session. */
    rc = h2_iostreamikcp_flush(connection->stream);
    if (rc != H2_PAL_OK) {
        return rc == H2_PAL_ERR_CLOSED ? H2_PAL_OK : rc;
    }
    rc = serial_now(connection->time, &start);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    now = start;
    while (now - start < H2_H2LOADER_HOST_SERIAL_RESPONSE_ACK_GRACE_MS) {
        uint32_t remaining = H2_H2LOADER_HOST_SERIAL_RESPONSE_ACK_GRACE_MS -
            (uint32_t)(now - start);
        uint32_t poll_ms = remaining < H2_H2LOADER_HOST_SERIAL_POLL_MS
            ? remaining
            : H2_H2LOADER_HOST_SERIAL_POLL_MS;
        rc = serial_pump(connection, poll_ms);
        if (rc == H2_PAL_ERR_CLOSED) {
            return H2_PAL_OK;
        }
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = serial_now(connection->time, &now);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_host_serial_execute_command(
    h2_h2loader_host_serial_connection_t *connection,
    const h2_h2loader_host_command_request_t *request,
    h2_h2loader_host_command_result_t *out_result) {
    return h2_h2loader_host_command_execute_transport(
        connection,
        serial_command_write,
        serial_command_read,
        serial_finish_command_response,
        request,
        out_result);
}

h2_pal_result_t h2_h2loader_host_serial_stage(
    h2_h2loader_host_serial_connection_t *connection,
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user,
    h2_h2loader_host_progress_fn on_progress,
    void *progress_user) {
    char command[192];
    uint8_t buffer[8192];
    uint8_t response[H2_H2LOADER_HOST_SERIAL_RESPONSE_SIZE];
    uint64_t offset = 0u;
    size_t response_len = 0u;

    if (connection == NULL || asset == NULL || read_payload == NULL ||
        asset->operation !=
            H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL ||
        asset->bytes == 0u || !h2_h2loader_host_is_sha256(asset->sha256)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    int command_len = snprintf(
        command,
        sizeof(command),
        "h2loader stage %llu %s\n",
        (unsigned long long)asset->bytes,
        asset->sha256);
    if (command_len <= 0 || (size_t)command_len >= sizeof(command)) {
        return H2_PAL_ERR_NO_SPACE;
    }
    h2_pal_result_t rc = h2_iostreamikcp_write(
        connection->stream,
        (const uint8_t *)command,
        (size_t)command_len);
    while (rc == H2_PAL_OK && offset < asset->bytes) {
        uint64_t batch_end = offset + H2_H2LOADER_HOST_SERIAL_WRITE_BATCH;
        if (batch_end > asset->bytes || batch_end < offset) {
            batch_end = asset->bytes;
        }
        while (rc == H2_PAL_OK && offset < batch_end) {
            size_t request = batch_end - offset > sizeof(buffer)
                ? sizeof(buffer)
                : (size_t)(batch_end - offset);
            size_t read = 0u;
            if (is_cancelled != NULL && is_cancelled(cancel_user)) {
                return H2_PAL_EXIT;
            }
            rc = read_payload(
                payload_user, offset, buffer, request, &read);
            if (rc != H2_PAL_OK) {
                return rc;
            }
            if (read == 0u || read > request) {
                return H2_PAL_ERR_TRUNCATED;
            }
            rc = h2_iostreamikcp_write(
                connection->stream, buffer, read);
            offset += read;
        }
        if (rc == H2_PAL_OK) {
            rc = serial_wait_delivery(
                connection, connection->command_timeout_ms + 30000u);
        }
        if (rc == H2_PAL_OK && on_progress != NULL) {
            on_progress(progress_user, offset, asset->bytes);
        }
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = serial_read_until(
        connection,
        "H2_LOADER_STAGE_RECEIVE result=",
        "H2_LOADER_STAGE result=",
        response,
        sizeof(response),
        &response_len,
        connection->command_timeout_ms + 30000u,
        NULL,
        NULL);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return marker_result_is_ok(
               response,
               response_len,
               "H2_LOADER_STAGE_RECEIVE result=") &&
            marker_result_is_ok(
                response, response_len, "H2_LOADER_STAGE result=")
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

h2_pal_result_t h2_h2loader_host_serial_activate(
    h2_h2loader_host_serial_connection_t *connection,
    const h2_h2loader_host_catalog_entry_t *asset) {
    uint8_t response[H2_H2LOADER_HOST_SERIAL_RESPONSE_SIZE];
    size_t response_len = 0u;

    if (connection == NULL || asset == NULL ||
        asset->operation !=
            H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (asset->role == H2_H2LOADER_HOST_ASSET_ROLE_APP) {
        h2_pal_result_t rc =
            serial_write_command(connection, "h2loader reboot\n");
        if (rc != H2_PAL_OK) {
            return rc;
        }
        rc = serial_read_until(
            connection,
            "H2_LOADER_REBOOT target=app result=accepted",
            NULL,
            response,
            sizeof(response),
            &response_len,
            connection->command_timeout_ms + 30000u,
            NULL,
            NULL);
        if (rc == H2_PAL_ERR_CLOSED &&
            response_has_complete_marker(
                response,
                response_len,
                "H2_LOADER_REBOOT target=app result=accepted")) {
            return H2_PAL_OK;
        }
        return rc;
    }
    if (asset->role != H2_H2LOADER_HOST_ASSET_ROLE_LOADER) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc =
        serial_write_command(connection, "h2loader upgrade\n");
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = serial_read_until(
        connection,
        "H2_LOADER_UPGRADE result=",
        NULL,
        response,
        sizeof(response),
        &response_len,
        connection->command_timeout_ms + 30000u,
        NULL,
        NULL);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return marker_result_is_ok(
        response, response_len, "H2_LOADER_UPGRADE result=")
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}
