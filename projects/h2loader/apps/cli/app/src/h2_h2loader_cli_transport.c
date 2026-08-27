#include "h2_h2loader_cli_internal.h"

#include <string.h>

#define H2_H2LOADER_CLI_BLE_CANDIDATE_CAPACITY 32u

static int serial_candidate_is_darwin_esp_usb_jtag(
    const h2_h2loader_host_candidate_t *candidate) {
#if defined(__APPLE__) && defined(__MACH__)
    const uint32_t usb_fields =
        H2_PAL_SERIAL_HOST_PORT_FIELD_USB_VID |
        H2_PAL_SERIAL_HOST_PORT_FIELD_USB_PID;
    return candidate != NULL &&
        (candidate->serial_valid_fields & usb_fields) == usb_fields &&
        candidate->usb_vid == 0x303au && candidate->usb_pid == 0x1001u;
#else
    (void)candidate;
    return 0;
#endif
}

static void select_serial_policy(h2_h2loader_cli_transport_t *transport) {
    transport->serial_control_line_mask =
        H2_PAL_SERIAL_HOST_CONTROL_DTR |
        H2_PAL_SERIAL_HOST_CONTROL_RTS;
    transport->serial_asserted_control_lines = 0u;
    if (transport->serial_candidate_valid &&
        serial_candidate_is_darwin_esp_usb_jtag(
            &transport->serial_candidate)) {
        transport->serial_control_line_mask = 0u;
    }
    transport->serial_policy_valid = 1u;
}

static h2_pal_result_t resolve_serial_candidate(
    h2_h2loader_cli_transport_t *transport) {
    const h2_pal_serial_host_api_t *serial = transport->context->config->serial;
    h2_pal_serial_host_snapshot_t *snapshot = NULL;
    h2_pal_result_t rc = h2_pal_serial_host_scan(serial, &snapshot);
    size_t count = 0u;

    if (rc == H2_PAL_OK) {
        rc = h2_pal_serial_host_snapshot_count(serial, snapshot, &count);
    }
    for (size_t index = 0u; rc == H2_PAL_OK && index < count; ++index) {
        h2_pal_serial_host_port_info_t info;
        rc = h2_pal_serial_host_snapshot_get(
            serial, snapshot, index, &info);
        if (rc != H2_PAL_OK ||
            strcmp(info.port_id, transport->options->port) != 0) {
            continue;
        }
        memset(&transport->serial_candidate, 0,
            sizeof(transport->serial_candidate));
        transport->serial_candidate.transport =
            H2_H2LOADER_HOST_TRANSPORT_SERIAL;
        memcpy(
            transport->serial_candidate.port_id,
            info.port_id,
            strlen(info.port_id) + 1u);
        transport->serial_candidate.serial_valid_fields = info.valid_fields;
        transport->serial_candidate.serial_capabilities = info.capabilities;
        transport->serial_candidate.usb_vid = info.usb_vid;
        transport->serial_candidate.usb_pid = info.usb_pid;
        transport->serial_candidate_valid = 1u;
        break;
    }
    h2_pal_result_t destroy_rc = h2_pal_serial_host_snapshot_destroy(
        serial, &snapshot);
    if (destroy_rc != H2_PAL_OK) {
        return destroy_rc;
    }
    return H2_PAL_OK;
}

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
    if (transport->ble_candidate_valid &&
        strcmp(
            transport->ble_candidate.candidate_id,
            selected.candidate_id) != 0) {
        return H2_PAL_ERR_INVALID_STATE;
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
}

h2_pal_result_t h2_h2loader_cli_transport_set_serial_candidate(
    h2_h2loader_cli_transport_t *transport,
    const h2_h2loader_host_candidate_t *candidate) {
    if (transport == NULL || transport->options == NULL ||
        transport->options->port == NULL || candidate == NULL ||
        candidate->transport != H2_H2LOADER_HOST_TRANSPORT_SERIAL ||
        strcmp(candidate->port_id, transport->options->port) != 0) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    transport->serial_candidate = *candidate;
    transport->serial_candidate_valid = 1u;
    transport->serial_policy_valid = 0u;
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_cli_transport_prepare_serial_policy(
    h2_h2loader_cli_transport_t *transport) {
    if (transport == NULL || transport->context == NULL ||
        transport->context->config == NULL || transport->options == NULL ||
        transport->options->port == NULL ||
        transport->options->transport != H2_H2LOADER_HOST_TRANSPORT_SERIAL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (transport->serial_policy_valid) {
        return H2_PAL_OK;
    }
    if (!transport->serial_candidate_valid) {
        h2_pal_result_t rc = resolve_serial_candidate(transport);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    select_serial_policy(transport);
    return H2_PAL_OK;
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
        };
        return h2_h2loader_host_ble_connect(
            &connect, &transport->ble_connection, out_status);
    }
    rc = h2_h2loader_cli_transport_prepare_serial_policy(transport);
    if (rc != H2_PAL_OK) return rc;
    h2_h2loader_host_serial_connection_config_t connect = {
        .serial = transport->context->config->serial,
        .time = transport->context->runtime->time,
        .allocator = transport->context->runtime->mem,
        .port_id = transport->options->port,
        .handshake_timeout_ms = transport->options->wait_timeout_ms,
        .command_timeout_ms = transport->command_timeout_ms,
        .initial_control_line_mask = transport->serial_control_line_mask,
        .initial_asserted_control_lines =
            transport->serial_asserted_control_lines,
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
    /* BLE v1/v2 has no authoritative physical device UID. A backend address
     * may select the initial candidate within this Host lifetime, but must not
     * be promoted into identity across a new scan or disconnected session. */
    return H2_PAL_ERR_UNSUPPORTED;
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
