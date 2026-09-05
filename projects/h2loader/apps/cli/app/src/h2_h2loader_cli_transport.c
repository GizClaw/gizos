#include "h2_h2loader_cli_internal.h"

#include <stdio.h>
#include <string.h>

#define H2_H2LOADER_CLI_BLE_CANDIDATE_CAPACITY 32u

static h2_pal_result_t resolve_ble_candidate(
    h2_h2loader_cli_transport_t *transport) {
    h2_h2loader_host_candidate_t candidates[
        H2_H2LOADER_CLI_BLE_CANDIDATE_CAPACITY];
    h2_h2loader_host_scan_result_t result;
    const h2_pal_ble_host_api_t *ble =
        h2_h2loader_cli_acquire_ble(transport->context);
    size_t matches = 0u;
    h2_h2loader_host_candidate_t selected;

    if (ble == NULL) return H2_PAL_ERR_UNSUPPORTED;
    h2_h2loader_host_scan_config_t scan = {
        .ble = ble,
        .sync = transport->context->runtime->sync,
        .time = transport->context->runtime->time,
        .ble_timeout_ms = transport->options->wait_timeout_ms,
        .ble_endpoint = transport->options->port,
        .candidates = candidates,
        .candidate_capacity = H2_H2LOADER_CLI_BLE_CANDIDATE_CAPACITY,
    };
    h2_pal_result_t rc = h2_h2loader_host_scan(&scan, &result);
    if (rc != H2_PAL_OK) return rc;
    memset(&selected, 0, sizeof(selected));
    for (size_t i = 0u; i < result.count; ++i) {
        if (candidates[i].transport == H2_H2LOADER_HOST_TRANSPORT_BLE &&
            strcmp(candidates[i].endpoint, transport->options->port) == 0) {
            selected = candidates[i];
            ++matches;
        }
    }
    if (matches != 1u) {
        return matches == 0u ? H2_PAL_ERR_NOT_FOUND : H2_PAL_ERR_INVALID_STATE;
    }
    transport->ble_candidate = selected;
    transport->ble_candidate_valid = 1u;
    return H2_PAL_OK;
}

void h2_h2loader_cli_transport_init(
    h2_h2loader_cli_transport_t *transport,
    h2_h2loader_cli_context_t *context,
    const h2_h2loader_cli_options_t *options,
    uint32_t command_timeout_ms) {
    memset(transport, 0, sizeof(*transport));
    transport->context = context;
    transport->options = options;
    transport->ready_marker = options->ready_marker;
    transport->command_timeout_ms = command_timeout_ms;
    if (options->transport != H2_H2LOADER_HOST_TRANSPORT_BLE) {
        transport->on_log = h2_h2loader_cli_transport_log;
        transport->log_user = context;
    }
}

h2_pal_result_t h2_h2loader_cli_transport_connect(
    h2_h2loader_cli_transport_t *transport,
    h2_h2loader_host_status_t *out_status) {
    h2_pal_result_t rc;
    if (transport == NULL || transport->context == NULL ||
        transport->options == NULL || out_status == NULL ||
        transport->options->port == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    (void)h2_h2loader_cli_transport_disconnect(transport);
    if (transport->options->transport == H2_H2LOADER_HOST_TRANSPORT_BLE) {
        if (!transport->ble_candidate_valid) {
            rc = resolve_ble_candidate(transport);
            if (rc != H2_PAL_OK) return rc;
        }
        const char *advertised_board =
            strncmp(
                transport->ble_candidate.advertised_board,
                "fnv1a64:", 8u) == 0
            ? NULL
            : transport->ble_candidate.advertised_board;
        h2_h2loader_host_ble_connection_config_t connect = {
            .ble = transport->context->runtime->ble_host,
            .task = transport->context->runtime->task,
            .time = transport->context->runtime->time,
            .sync = transport->context->runtime->sync,
            .system_event = transport->context->runtime->system_event,
            .allocator = transport->context->runtime->mem,
            .address = transport->ble_candidate.ble_address,
            .advertised_board = advertised_board,
            .connect_timeout_ms = transport->options->wait_timeout_ms,
            .command_timeout_ms = transport->command_timeout_ms,
            .on_log = transport->on_log,
            .log_user = transport->log_user,
        };
        rc = h2_h2loader_host_ble_connect(
            &connect, &transport->ble_connection, out_status);
        if (rc == H2_PAL_OK &&
            !transport->authoritative_device_uid_valid) {
            if (out_status->device_uid[0] == '\0' ||
                strcmp(out_status->device_uid, "unknown") == 0) {
                return H2_PAL_OK;
            }
            (void)snprintf(transport->authoritative_device_uid,
                           sizeof(transport->authoritative_device_uid), "%s",
                           out_status->device_uid);
            transport->authoritative_device_uid_valid = 1u;
        } else if (rc == H2_PAL_OK &&
                   strcmp(transport->authoritative_device_uid,
                          out_status->device_uid) != 0) {
            rc = H2_PAL_ERR_INVALID_STATE;
        }
        if (rc != H2_PAL_OK) {
            (void)h2_h2loader_cli_transport_disconnect(transport);
        }
        return rc;
    }
    h2_h2loader_host_serial_connection_config_t connect = {
        .serial = transport->context->config->serial,
        .time = transport->context->runtime->time,
        .allocator = transport->context->runtime->mem,
        .port_id = transport->options->port,
        .baud_rate = transport->options->baud_rate,
        .handshake_timeout_ms = transport->options->wait_timeout_ms,
        .command_timeout_ms = transport->command_timeout_ms,
        .ready_marker = transport->ready_marker,
        .post_command_delay_ms = transport->options->post_delay_ms,
        .on_log = transport->on_log,
        .log_user = transport->log_user,
    };
    rc = h2_h2loader_host_serial_connect(
        &connect, &transport->serial_connection);
    if (rc == H2_PAL_OK) transport->ready_marker = NULL;
    if (rc == H2_PAL_OK) {
        rc = h2_h2loader_host_serial_read_status(
            transport->serial_connection, out_status);
    }
    if (rc != H2_PAL_OK) {
        (void)h2_h2loader_cli_transport_disconnect(transport);
    }
    return rc;
}

h2_pal_result_t h2_h2loader_cli_transport_execute(
    h2_h2loader_cli_transport_t *transport,
    const h2_h2loader_host_command_request_t *request,
    h2_h2loader_host_command_result_t *out_result) {
    if (transport->options->transport == H2_H2LOADER_HOST_TRANSPORT_BLE) {
        return h2_h2loader_host_ble_execute_command(
            transport->ble_connection, request, out_result);
    }
    return h2_h2loader_host_serial_execute_command(
        transport->serial_connection, request, out_result);
}

h2_pal_result_t h2_h2loader_cli_transport_stage(
    h2_h2loader_cli_transport_t *transport,
    const h2_h2loader_host_catalog_entry_t *asset,
    h2_h2loader_host_payload_read_fn read_payload,
    void *payload_user,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user,
    h2_h2loader_host_progress_fn on_progress,
    void *progress_user) {
    if (transport->options->transport == H2_H2LOADER_HOST_TRANSPORT_BLE) {
        return h2_h2loader_host_ble_stage(
            transport->ble_connection, asset, read_payload, payload_user,
            is_cancelled, cancel_user, on_progress, progress_user);
    }
    return h2_h2loader_host_serial_stage(
        transport->serial_connection, asset, read_payload, payload_user,
        is_cancelled, cancel_user, on_progress, progress_user);
}

h2_pal_result_t h2_h2loader_cli_transport_read_status(
    h2_h2loader_cli_transport_t *transport,
    h2_h2loader_host_status_t *out_status) {
    if (transport == NULL || transport->options == NULL ||
        out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (transport->options->transport == H2_H2LOADER_HOST_TRANSPORT_BLE) {
        return h2_h2loader_host_ble_read_status(
            transport->ble_connection, out_status);
    }
    return h2_h2loader_host_serial_read_status(
        transport->serial_connection, out_status);
}

h2_pal_result_t h2_h2loader_cli_transport_disconnect(
    h2_h2loader_cli_transport_t *transport) {
    h2_pal_result_t serial_rc = h2_h2loader_host_serial_disconnect(
        &transport->serial_connection);
    h2_pal_result_t ble_rc = h2_h2loader_host_ble_disconnect(
        &transport->ble_connection);
    return serial_rc == H2_PAL_OK ? ble_rc : serial_rc;
}

h2_pal_result_t h2_h2loader_cli_transport_rediscover(
    h2_h2loader_cli_transport_t *transport) {
    if (transport->options->transport != H2_H2LOADER_HOST_TRANSPORT_BLE) {
        return H2_PAL_OK;
    }
    if (!transport->authoritative_device_uid_valid) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return resolve_ble_candidate(transport);
}

int h2_h2loader_cli_reconnect_must_fail_closed(
    const h2_h2loader_cli_transport_t *transport,
    h2_pal_result_t connect_result) {
    return transport != NULL && transport->options != NULL &&
        transport->options->transport == H2_H2LOADER_HOST_TRANSPORT_BLE &&
        connect_result == H2_PAL_ERR_INVALID_STATE;
}

h2_pal_result_t h2_h2loader_cli_transport_monitor_logs(
    h2_h2loader_cli_transport_t *transport,
    h2_h2loader_host_cancelled_fn is_cancelled,
    void *cancel_user) {
    if (transport->options->transport == H2_H2LOADER_HOST_TRANSPORT_BLE) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return h2_h2loader_host_serial_monitor_logs(
        transport->serial_connection, is_cancelled, cancel_user);
}
