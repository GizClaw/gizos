#include "h2_h2loader_host.h"

#include "h2_bleikcp.h"
#include "h2_h2loader_host_internal.h"
#include "h2_h2loader_host_task_names.h"

#include <stdio.h>
#include <string.h>

#define H2_H2LOADER_HOST_BLE_RESPONSE_SIZE 8192u
#define H2_H2LOADER_HOST_BLE_BUFFER_SIZE (32u * 1024u)
#define H2_H2LOADER_HOST_BLE_WINDOW 32u
#define H2_H2LOADER_HOST_BLE_INPUT_CAPACITY 64u
#define H2_H2LOADER_HOST_BLE_IO_CHUNK 8192u

static const uint8_t service_uuid[16] = {
    0x1du, 0x72u, 0xa1u, 0x6bu, 0x3au, 0xafu, 0x0bu, 0xaau,
    0xe2u, 0x53u, 0xd8u, 0x3eu, 0x70u, 0xb5u, 0xa4u, 0x71u,
};
static const uint8_t tx_uuid[16] = {
    0x1eu, 0xcfu, 0xd2u, 0xbcu, 0x8fu, 0xd3u, 0xb0u, 0x98u,
    0x70u, 0x51u, 0xfbu, 0x56u, 0x55u, 0xa0u, 0xd3u, 0x46u,
};
static const uint8_t rx_uuid[16] = {
    0xfeu, 0x0eu, 0xbcu, 0xc9u, 0xd6u, 0x87u, 0x36u, 0xa5u,
    0x8du, 0x5bu, 0xf2u, 0x05u, 0x15u, 0xadu, 0x62u, 0x8fu,
};

struct h2_h2loader_host_ble_connection {
    const h2_pal_ble_host_api_t *ble;
    const h2_pal_time_api_t *time;
    const h2_pal_mem_api_t *allocator;
    h2_bleikcp_t *stream;
    uint16_t conn_handle;
    uint32_t command_timeout_ms;
};

static h2_pal_result_t ble_deadline_after(
    const h2_pal_time_api_t *time,
    uint32_t timeout_ms,
    uint64_t *out_deadline_ms) {
    uint64_t now_ms = 0u;
    h2_pal_result_t rc =
        h2_pal_time_get_monotonic_ms(time, &now_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    *out_deadline_ms = UINT64_MAX - now_ms < timeout_ms
        ? UINT64_MAX
        : now_ms + timeout_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t ble_remaining_timeout(
    const h2_pal_time_api_t *time,
    uint64_t deadline_ms,
    uint32_t *out_timeout_ms) {
    uint64_t now_ms = 0u;
    h2_pal_result_t rc =
        h2_pal_time_get_monotonic_ms(time, &now_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (now_ms >= deadline_ms) {
        return H2_PAL_ERR_TIMEOUT;
    }
    uint64_t remaining_ms = deadline_ms - now_ms;
    *out_timeout_ms = remaining_ms > UINT32_MAX
        ? UINT32_MAX
        : (uint32_t)remaining_ms;
    return H2_PAL_OK;
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

static h2_pal_result_t ble_write(
    h2_h2loader_host_ble_connection_t *connection,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    while (len > 0u) {
        size_t chunk =
            len > H2_H2LOADER_HOST_BLE_IO_CHUNK
                ? H2_H2LOADER_HOST_BLE_IO_CHUNK
                : len;
        h2_pal_result_t rc = h2_bleikcp_write(
            connection->stream, data, chunk, timeout_ms);
        if (rc != H2_PAL_OK) {
            return rc;
        }
        data += chunk;
        len -= chunk;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t ble_write_command(
    h2_h2loader_host_ble_connection_t *connection,
    const char *command) {
    h2_pal_result_t rc = ble_write(
        connection,
        (const uint8_t *)command,
        strlen(command),
        connection->command_timeout_ms);
    return rc == H2_PAL_OK
        ? h2_bleikcp_flush(
              connection->stream, connection->command_timeout_ms)
        : rc;
}

static h2_pal_result_t ble_read_until(
    h2_h2loader_host_ble_connection_t *connection,
    const char *marker_a,
    const char *marker_b,
    uint8_t *response,
    size_t response_size,
    size_t *out_len,
    uint32_t timeout_ms,
    h2_h2loader_host_command_output_fn on_output,
    void *output_user,
    uint8_t stream_output,
    size_t *out_output_bytes) {
    uint8_t input[1024];
    size_t length = 0u;
    size_t output_bytes = 0u;
    uint64_t deadline_ms = 0u;
    *out_len = 0u;
    if (out_output_bytes != NULL) *out_output_bytes = 0u;
    h2_pal_result_t rc = ble_deadline_after(
        connection->time, timeout_ms, &deadline_ms);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    for (;;) {
        uint32_t remaining_ms = 0u;
        rc = ble_remaining_timeout(
            connection->time, deadline_ms, &remaining_ms);
        if (rc != H2_PAL_OK) {
            *out_len = length;
            return rc;
        }
        size_t read = 0u;
        rc = h2_bleikcp_read(
            connection->stream,
            input,
            sizeof(input),
            &read,
            remaining_ms);
        if (rc != H2_PAL_OK) {
            *out_len = length;
            if (out_output_bytes != NULL) *out_output_bytes = output_bytes;
            return rc;
        }
        if (read == 0u) {
            *out_len = length;
            if (out_output_bytes != NULL) *out_output_bytes = output_bytes;
            return H2_PAL_ERR_TIMEOUT;
        }
        output_bytes += read;
        if (on_output != NULL) {
            rc = on_output(output_user, input, read);
            if (rc != H2_PAL_OK) {
                *out_len = length;
                if (out_output_bytes != NULL) *out_output_bytes = output_bytes;
                return rc;
            }
        }
        if (stream_output) {
            if (read >= response_size) {
                memcpy(response, &input[read - response_size], response_size);
                length = response_size;
            } else {
                size_t discard = length + read > response_size
                    ? length + read - response_size : 0u;
                if (discard != 0u) {
                    memmove(response, &response[discard], length - discard);
                    length -= discard;
                }
                memcpy(&response[length], input, read);
                length += read;
            }
        } else {
            size_t capture = read > response_size - length
                ? response_size - length : read;
            memcpy(&response[length], input, capture);
            length += capture;
            if (capture != read) {
                *out_len = length;
                if (out_output_bytes != NULL) *out_output_bytes = output_bytes;
                return H2_PAL_ERR_NO_SPACE;
            }
        }
        if (response_has_complete_marker(response, length, marker_a) &&
            (marker_b == NULL ||
             response_has_complete_marker(response, length, marker_b))) {
            *out_len = length;
            if (out_output_bytes != NULL) *out_output_bytes = output_bytes;
            return H2_PAL_OK;
        }
    }
}

static void connect_diagnostic(
    const h2_h2loader_host_ble_connection_config_t *config,
    const char *stage, h2_pal_result_t result) {
    if (config->on_log == NULL) return;
    char line[96];
    const int length = snprintf(
        line, sizeof(line), "H2_BLE_HOST_DIAG stage=%s rc=%d\n", stage, result);
    if (length > 0 && (size_t)length < sizeof(line)) {
        (void)config->on_log(
            config->log_user, (const uint8_t *)line, (size_t)length);
    }
}

h2_pal_result_t h2_h2loader_host_ble_connect(
    const h2_h2loader_host_ble_connection_config_t *config,
    h2_h2loader_host_ble_connection_t **out_connection,
    h2_h2loader_host_status_t *out_status) {
    h2_h2loader_host_ble_connection_t *connection = NULL;
    uint32_t timeout_ms;
    uint16_t mtu = 0u;
    h2_pal_result_t rc;

    if (out_connection != NULL) {
        *out_connection = NULL;
    }
    if (out_status != NULL) {
        memset(out_status, 0, sizeof(*out_status));
    }
    if (config == NULL || out_connection == NULL || out_status == NULL ||
        config->ble == NULL || config->task == NULL ||
        config->time == NULL || config->sync == NULL ||
        config->system_event == NULL || config->allocator == NULL ||
        config->address.type == H2_PAL_BLE_ADDR_TYPE_UNKNOWN) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    connection = h2_pal_mem_alloc(
        config->allocator, sizeof(*connection));
    if (connection == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    memset(connection, 0, sizeof(*connection));
    connection->ble = config->ble;
    connection->time = config->time;
    connection->allocator = config->allocator;
    connection->conn_handle = H2_PAL_BLE_INVALID_CONN_HANDLE;
    connection->command_timeout_ms =
        config->command_timeout_ms == 0u
            ? H2_H2LOADER_HOST_DEFAULT_COMMAND_TIMEOUT_MS
            : config->command_timeout_ms;
    timeout_ms = config->connect_timeout_ms == 0u
        ? H2_H2LOADER_HOST_DEFAULT_COMMAND_TIMEOUT_MS
        : config->connect_timeout_ms;
    const h2_pal_ble_connect_params_t connect_params = {
        .timeout_ms = timeout_ms,
        .interval_min_ms = 15u,
        .interval_max_ms = 30u,
        .latency = 0u,
        .supervision_timeout_ms = 4000u,
    };
    rc = h2_pal_ble_connect(
        config->ble,
        &config->address,
        &connect_params,
        &connection->conn_handle);
    if (rc != H2_PAL_OK) {
        connect_diagnostic(config, "connect", rc);
        goto fail;
    }
    rc = h2_pal_ble_exchange_mtu(
        config->ble, connection->conn_handle, &mtu, timeout_ms);
    if (rc != H2_PAL_OK) {
        connect_diagnostic(config, "mtu", rc);
        goto fail;
    }
    const h2_bleikcp_api_t api = {
        .ble = config->ble,
        .task = config->task,
        .time = config->time,
        .sync = config->sync,
        .system_event = config->system_event,
        .allocator = config->allocator,
    };
    const h2_bleikcp_config_t stream_config = {
        .service_uuid = { service_uuid, sizeof(service_uuid) },
        .tx_char_uuid = { tx_uuid, sizeof(tx_uuid) },
        .rx_char_uuid = { rx_uuid, sizeof(rx_uuid) },
        .send_window = H2_H2LOADER_HOST_BLE_WINDOW,
        .recv_window = H2_H2LOADER_HOST_BLE_WINDOW,
        .input_frame_capacity = H2_H2LOADER_HOST_BLE_INPUT_CAPACITY,
        .tx_buffer_size = H2_H2LOADER_HOST_BLE_BUFFER_SIZE,
        .rx_buffer_size = H2_H2LOADER_HOST_BLE_BUFFER_SIZE,
        .no_congestion_control = 0,
        .output_retry_count = 40u,
        .output_retry_delay_ms = 2u,
        .setup_timeout_ms = timeout_ms,
        .worker_task_options = {
            .name = h2_h2loader_host_ble_task_name,
            .min_stack_size = 8u * 1024u,
        },
    };
    rc = h2_bleikcp_client_open(
        &api,
        &stream_config,
        connection->conn_handle,
        mtu,
        &connection->stream);
    if (rc != H2_PAL_OK) {
        connect_diagnostic(config, "bleikcp-open", rc);
        goto fail;
    }
    rc = h2_h2loader_host_ble_read_status(connection, out_status);
    if (rc != H2_PAL_OK) {
        connect_diagnostic(config, "status", rc);
        goto fail;
    }
    if (config->advertised_board != NULL &&
        config->advertised_board[0] != '\0' &&
        strcmp(config->advertised_board, out_status->board) != 0) {
        rc = H2_PAL_ERR_INVALID_STATE;
        goto fail;
    }
    *out_connection = connection;
    return H2_PAL_OK;

fail:
    if (connection->stream != NULL) {
        (void)h2_bleikcp_close(connection->stream);
        connection->stream = NULL;
    }
    if (connection->conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE) {
        (void)h2_pal_ble_disconnect(
            connection->ble, connection->conn_handle);
    }
    h2_pal_mem_free(config->allocator, connection);
    return rc;
}

h2_pal_result_t h2_h2loader_host_ble_disconnect(
    h2_h2loader_host_ble_connection_t **inout_connection) {
    h2_h2loader_host_ble_connection_t *connection;
    h2_pal_result_t stream_rc = H2_PAL_OK;
    h2_pal_result_t disconnect_rc = H2_PAL_OK;
    if (inout_connection == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    connection = *inout_connection;
    if (connection == NULL) {
        return H2_PAL_OK;
    }
    *inout_connection = NULL;
    if (connection->stream != NULL) {
        stream_rc = h2_bleikcp_close(connection->stream);
        connection->stream = NULL;
    }
    if (connection->conn_handle != H2_PAL_BLE_INVALID_CONN_HANDLE) {
        disconnect_rc = h2_pal_ble_disconnect(
            connection->ble, connection->conn_handle);
    }
    const h2_pal_mem_api_t *allocator = connection->allocator;
    h2_pal_mem_free(allocator, connection);
    /* Teardown is idempotent. CoreBluetooth reports UNSUPPORTED when the
       peripheral has already disappeared, while stream teardown can report
       CLOSED for the same completed state. Neither means local resources are
       still live or that the preceding device operation failed. */
    if (stream_rc == H2_PAL_ERR_CLOSED ||
        stream_rc == H2_PAL_ERR_NOT_FOUND ||
        stream_rc == H2_PAL_ERR_UNSUPPORTED) {
        stream_rc = H2_PAL_OK;
    }
    if (disconnect_rc == H2_PAL_ERR_CLOSED ||
        disconnect_rc == H2_PAL_ERR_NOT_FOUND ||
        disconnect_rc == H2_PAL_ERR_UNSUPPORTED) {
        disconnect_rc = H2_PAL_OK;
    }
    return stream_rc != H2_PAL_OK ? stream_rc : disconnect_rc;
}

h2_pal_result_t h2_h2loader_host_ble_read_status(
    h2_h2loader_host_ble_connection_t *connection,
    h2_h2loader_host_status_t *out_status) {
    uint8_t response[H2_H2LOADER_HOST_BLE_RESPONSE_SIZE];
    size_t response_len = 0u;
    static const char loader_marker[] = "H2_LOADER_STATUS ";
    if (out_status != NULL) {
        memset(out_status, 0, sizeof(*out_status));
    }
    if (connection == NULL || out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc =
        ble_write_command(connection, "h2loader status\n");
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = ble_read_until(
        connection,
        loader_marker,
        NULL,
        response,
        sizeof(response) - 1u,
        &response_len,
        connection->command_timeout_ms,
        NULL,
        NULL,
        0u,
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

static h2_pal_result_t ble_command_write(
    void *transport,
    const char *line) {
    return ble_write_command(transport, line);
}

static h2_pal_result_t ble_command_read(
    void *transport,
    const char *marker,
    uint8_t *response,
    size_t response_size,
    size_t *out_response_len,
    h2_h2loader_host_command_output_fn on_output,
    void *output_user,
    uint8_t stream_output,
    size_t *out_output_bytes) {
    h2_h2loader_host_ble_connection_t *connection = transport;
    return ble_read_until(
        connection,
        marker,
        NULL,
        response,
        response_size,
        out_response_len,
        connection->command_timeout_ms + 30000u,
        on_output,
        output_user,
        stream_output,
        out_output_bytes);
}

h2_pal_result_t h2_h2loader_host_ble_execute_command(
    h2_h2loader_host_ble_connection_t *connection,
    const h2_h2loader_host_command_request_t *request,
    h2_h2loader_host_command_result_t *out_result) {
    return h2_h2loader_host_command_execute_transport(
        connection,
        ble_command_write,
        ble_command_read,
        NULL,
        request,
        out_result);
}

h2_pal_result_t h2_h2loader_host_ble_stage(
    h2_h2loader_host_ble_connection_t *connection,
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user,
    h2_h2loader_host_progress_fn on_progress,
    void *progress_user) {
    char command[192];
    uint8_t buffer[H2_H2LOADER_HOST_BLE_IO_CHUNK];
    uint8_t response[H2_H2LOADER_HOST_BLE_RESPONSE_SIZE];
    uint64_t offset = 0u;
    size_t response_len = 0u;
    if (connection == NULL || asset == NULL || read_payload == NULL ||
        asset->operation !=
            H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL ||
        asset->bytes == 0u ||
        !h2_h2loader_host_is_sha256(asset->sha256)) {
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
    h2_pal_result_t rc = ble_write(
        connection,
        (const uint8_t *)command,
        (size_t)command_len,
        connection->command_timeout_ms);
    while (rc == H2_PAL_OK && offset < asset->bytes) {
        size_t request =
            asset->bytes - offset > sizeof(buffer)
                ? sizeof(buffer)
                : (size_t)(asset->bytes - offset);
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
        rc = ble_write(
            connection,
            buffer,
            read,
            connection->command_timeout_ms + 30000u);
        offset += read;
        if (rc == H2_PAL_OK && on_progress != NULL) {
            on_progress(progress_user, offset, asset->bytes);
        }
    }
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_bleikcp_flush(
        connection->stream, connection->command_timeout_ms + 30000u);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = ble_read_until(
        connection,
        "H2_LOADER_STAGE_RECEIVE result=",
        "H2_LOADER_STAGE result=",
        response,
        sizeof(response),
        &response_len,
        connection->command_timeout_ms + 30000u,
        NULL,
        NULL,
        0u,
        NULL);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    return marker_result_is_ok(
               response,
               response_len,
               "H2_LOADER_STAGE_RECEIVE result=") &&
            marker_result_is_ok(
                response,
                response_len,
                "H2_LOADER_STAGE result=")
        ? H2_PAL_OK
        : H2_PAL_ERR_IO;
}

h2_pal_result_t h2_h2loader_host_ble_activate(
    h2_h2loader_host_ble_connection_t *connection,
    const h2_h2loader_host_catalog_entry_t *asset) {
    static const char accepted[] =
        "H2_LOADER_REBOOT target=upgrade result=accepted";
    uint8_t response[H2_H2LOADER_HOST_BLE_RESPONSE_SIZE];
    size_t response_len = 0u;
    if (connection == NULL || asset == NULL ||
        asset->operation !=
            H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (asset->role != H2_H2LOADER_HOST_ASSET_ROLE_APP &&
        asset->role != H2_H2LOADER_HOST_ASSET_ROLE_LOADER) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc =
        ble_write_command(connection, "h2loader reboot upgrade\n");
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = ble_read_until(
        connection,
        accepted,
        NULL,
        response,
        sizeof(response),
        &response_len,
        connection->command_timeout_ms + 30000u,
        NULL,
        NULL,
        0u,
        NULL);
    if ((rc == H2_PAL_ERR_CLOSED || rc == H2_PAL_ERR_TIMEOUT) &&
        response_has_complete_marker(response, response_len, accepted)) {
        return H2_PAL_OK;
    }
    return rc;
}
