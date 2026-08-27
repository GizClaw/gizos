#include "h2_h2loader_host_internal.h"

#include <stdio.h>
#include <string.h>

static int is_wire_field(const char *value) {
    const unsigned char *cursor = (const unsigned char *)value;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    while (*cursor != 0u) {
        if (*cursor <= 0x20u || *cursor == 0x7fu) {
            return 0;
        }
        ++cursor;
    }
    return 1;
}

h2_pal_result_t h2_h2loader_host_command_validate(
    const h2_h2loader_host_status_t *status,
    uint8_t operation_active,
    h2_h2loader_host_command_t command) {
    uint32_t availability = 0u;
    int lifecycle = 0;

    if (status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    switch (command) {
        case H2_H2LOADER_HOST_COMMAND_HELP:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_HELP;
            break;
        case H2_H2LOADER_HOST_COMMAND_STATUS:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_STATUS;
            break;
        case H2_H2LOADER_HOST_COMMAND_STATS:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_STATS;
            break;
        case H2_H2LOADER_HOST_COMMAND_MEMORY:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_MEMORY;
            break;
        case H2_H2LOADER_HOST_COMMAND_APP_RESTART:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_APP_RESTART;
            lifecycle = 1;
            break;
        case H2_H2LOADER_HOST_COMMAND_APP_ROLLBACK:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_APP_ROLLBACK;
            lifecycle = 1;
            break;
        case H2_H2LOADER_HOST_COMMAND_LOADER_REBOOT_APP:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_APP;
            lifecycle = 1;
            break;
        case H2_H2LOADER_HOST_COMMAND_LOADER_REBOOT_LOADER:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_LOADER;
            lifecycle = 1;
            break;
        case H2_H2LOADER_HOST_COMMAND_COREDUMP_STATUS:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_COREDUMP_STATUS;
            break;
        case H2_H2LOADER_HOST_COMMAND_COREDUMP_DUMP:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_COREDUMP_DUMP;
            break;
        case H2_H2LOADER_HOST_COMMAND_COREDUMP_ERASE:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_COREDUMP_ERASE;
            break;
        case H2_H2LOADER_HOST_COMMAND_STAGE_ABORT:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_STAGE_ABORT;
            break;
        case H2_H2LOADER_HOST_COMMAND_STAGE_URL:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_STAGE_URL;
            break;
        case H2_H2LOADER_HOST_COMMAND_HOLD_ON:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_HOLD_ON;
            break;
        case H2_H2LOADER_HOST_COMMAND_HOLD_OFF:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_HOLD_OFF;
            break;
        case H2_H2LOADER_HOST_COMMAND_WIFI_SCAN:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_WIFI_SCAN;
            break;
        case H2_H2LOADER_HOST_COMMAND_WIFI_CONNECT:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_WIFI_CONNECT;
            break;
        case H2_H2LOADER_HOST_COMMAND_WIFI_DISCONNECT:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_WIFI_DISCONNECT;
            break;
        case H2_H2LOADER_HOST_COMMAND_LOADER_UPGRADE:
            availability = H2_H2LOADER_HOST_COMMAND_AVAILABLE_LOADER_UPGRADE;
            lifecycle = 1;
            break;
        default:
            return H2_PAL_ERR_INVALID_ARG;
    }
    if (lifecycle && operation_active != 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    if ((status->command_availability & availability) == 0u) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_h2loader_host_command_contract(
    const h2_h2loader_host_command_request_t *request,
    h2_h2loader_host_command_contract_t *out_contract) {
    h2_pal_result_t rc;

    if (request == NULL || out_contract == NULL ||
        request->status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_contract, 0, sizeof(*out_contract));
    rc = h2_h2loader_host_command_validate(
        request->status, request->operation_active, request->command);
    if (rc != H2_PAL_OK) {
        return rc;
    }
#define SET_LINE(value) (void)snprintf(out_contract->line, sizeof(out_contract->line), "%s", (value))
    switch (request->command) {
        case H2_H2LOADER_HOST_COMMAND_HELP:
            SET_LINE("h2loader help\n");
            out_contract->marker = "h2loader <";
            out_contract->marker_is_success = 1u;
            break;
        case H2_H2LOADER_HOST_COMMAND_STATUS:
            /* Both Loader and App roles emit the structured
             * H2_LOADER_STATUS line; the line itself has no result
             * token, so its presence is the success terminal. */
            SET_LINE("h2loader status\n");
            out_contract->marker = "H2_LOADER_STATUS ";
            out_contract->marker_is_success = 1u;
            break;
        case H2_H2LOADER_HOST_COMMAND_STATS:
            SET_LINE("h2loader stats\n");
            out_contract->marker = "H2_LOADER_STATUS ";
            out_contract->marker_is_success = 1u;
            break;
        case H2_H2LOADER_HOST_COMMAND_MEMORY:
            SET_LINE("h2loader memory\n");
            out_contract->marker = "H2_LOADER_MEMORY ";
            out_contract->success_token = "result=OK";
            break;
        case H2_H2LOADER_HOST_COMMAND_APP_RESTART:
            SET_LINE("h2loader restart\n");
            out_contract->marker = "H2_LOADER_RESTART ";
            out_contract->success_token = "result=OK";
            /* The device prints result=OK then immediately reboots, dropping
             * the USB CDC link (surfaced as CLOSED). Accept the acknowledged
             * line even when the reboot disconnects the follow-up read. */
            out_contract->accepted_disconnect_token =
                "H2_LOADER_RESTART result=OK";
            out_contract->lifecycle_transition = 1u;
            break;
        case H2_H2LOADER_HOST_COMMAND_APP_ROLLBACK:
            SET_LINE("h2loader rollback\n");
            out_contract->marker = "H2_LOADER_ROLLBACK ";
            out_contract->success_token = "result=OK";
            out_contract->accepted_disconnect_token =
                "H2_LOADER_ROLLBACK result=OK";
            out_contract->lifecycle_transition = 1u;
            break;
        case H2_H2LOADER_HOST_COMMAND_LOADER_REBOOT_APP:
            SET_LINE("h2loader reboot app\n");
            out_contract->marker = "H2_LOADER_REBOOT ";
            out_contract->success_token = "result=accepted";
            out_contract->accepted_disconnect_token =
                "H2_LOADER_REBOOT target=app result=accepted";
            out_contract->lifecycle_transition = 1u;
            break;
        case H2_H2LOADER_HOST_COMMAND_LOADER_REBOOT_LOADER:
            SET_LINE("h2loader reboot loader\n");
            out_contract->marker = "H2_LOADER_REBOOT ";
            out_contract->success_token = "result=accepted";
            out_contract->accepted_disconnect_token =
                "H2_LOADER_REBOOT target=loader result=accepted";
            out_contract->lifecycle_transition = 1u;
            break;
        case H2_H2LOADER_HOST_COMMAND_COREDUMP_STATUS:
            SET_LINE("h2loader coredump status\n");
            out_contract->marker = "H2_LOADER_COREDUMP_STATUS ";
            out_contract->success_token = "result=OK";
            break;
        case H2_H2LOADER_HOST_COMMAND_COREDUMP_DUMP:
            SET_LINE("h2loader coredump dump\n");
            out_contract->marker = "H2_LOADER_COREDUMP_DUMP ";
            out_contract->success_token = "result=OK";
            break;
        case H2_H2LOADER_HOST_COMMAND_STAGE_ABORT:
            SET_LINE("h2loader stage abort\n");
            out_contract->marker = "H2_LOADER_STAGE_ABORT ";
            out_contract->success_token = "result=OK";
            break;
        case H2_H2LOADER_HOST_COMMAND_HOLD_ON:
        case H2_H2LOADER_HOST_COMMAND_HOLD_OFF:
            (void)snprintf(out_contract->line, sizeof(out_contract->line),
                "h2loader hold %s\n",
                request->command == H2_H2LOADER_HOST_COMMAND_HOLD_ON ? "on" : "off");
            out_contract->marker = "H2_LOADER_HOLD ";
            out_contract->success_token = "result=OK";
            break;
        case H2_H2LOADER_HOST_COMMAND_WIFI_SCAN: {
            uint32_t limit = request->wifi_scan_limit == 0u
                ? H2_H2LOADER_HOST_WIFI_SCAN_DEFAULT_LIMIT
                : request->wifi_scan_limit;
            uint32_t timeout_ms = request->wifi_scan_timeout_ms == 0u
                ? H2_H2LOADER_HOST_WIFI_SCAN_DEFAULT_TIMEOUT_MS
                : request->wifi_scan_timeout_ms;
            if (limit > H2_H2LOADER_HOST_WIFI_SCAN_MAX_LIMIT ||
                timeout_ms > H2_H2LOADER_HOST_WIFI_SCAN_MAX_TIMEOUT_MS) {
                return H2_PAL_ERR_INVALID_ARG;
            }
            if (snprintf(out_contract->line, sizeof(out_contract->line),
                    "h2loader wifi scan --limit %u --timeout-ms %u\n",
                    (unsigned)limit,
                    (unsigned)timeout_ms) >= (int)sizeof(out_contract->line)) {
                return H2_PAL_ERR_INVALID_ARG;
            }
            out_contract->marker = "H2_LOADER_WIFI_SCAN_DONE ";
            out_contract->success_token = "result=OK";
            break;
        }
        case H2_H2LOADER_HOST_COMMAND_WIFI_CONNECT:
            if (!is_wire_field(request->ssid) || !is_wire_field(request->password)) {
                return H2_PAL_ERR_INVALID_ARG;
            }
            if (snprintf(out_contract->line, sizeof(out_contract->line),
                    "h2loader wifi connect %s %s\n", request->ssid, request->password) >=
                (int)sizeof(out_contract->line)) {
                return H2_PAL_ERR_INVALID_ARG;
            }
            out_contract->marker = "H2_LOADER_WIFI ";
            out_contract->success_token = "result=connected";
            break;
        case H2_H2LOADER_HOST_COMMAND_WIFI_DISCONNECT:
            SET_LINE("h2loader wifi disconnect\n");
            out_contract->marker = "H2_LOADER_WIFI ";
            out_contract->success_token = "result=disconnected";
            break;
        case H2_H2LOADER_HOST_COMMAND_LOADER_UPGRADE:
            SET_LINE("h2loader upgrade\n");
            out_contract->marker = "H2_LOADER_UPGRADE ";
            out_contract->success_token = "result=OK";
            out_contract->accepted_disconnect_token =
                "H2_LOADER_UPGRADE result=OK";
            out_contract->lifecycle_transition = 1u;
            break;
        case H2_H2LOADER_HOST_COMMAND_COREDUMP_ERASE:
            SET_LINE("h2loader coredump erase\n");
            out_contract->marker = "H2_LOADER_COREDUMP_ERASE ";
            out_contract->success_token = "result=OK";
            break;
        case H2_H2LOADER_HOST_COMMAND_STAGE_URL:
            if (!is_wire_field(request->url) || request->expected_bytes == 0u ||
                !h2_h2loader_host_is_sha256(request->expected_sha256)) {
                return H2_PAL_ERR_INVALID_ARG;
            }
            if (snprintf(out_contract->line, sizeof(out_contract->line),
                    "h2loader stage url %s %llu %s\n", request->url,
                    (unsigned long long)request->expected_bytes,
                    request->expected_sha256) >= (int)sizeof(out_contract->line)) {
                return H2_PAL_ERR_INVALID_ARG;
            }
            out_contract->marker = "H2_LOADER_STAGE ";
            out_contract->success_token = "result=OK";
            break;
        default:
            return H2_PAL_ERR_INVALID_ARG;
    }
#undef SET_LINE
    return H2_PAL_OK;
}

static int contains(
    const uint8_t *response,
    size_t response_len,
    const char *value) {
    const size_t value_len = strlen(value);
    if (value_len == 0u || response_len < value_len) {
        return 0;
    }
    for (size_t i = 0u; i + value_len <= response_len; ++i) {
        if (memcmp(&response[i], value, value_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static const uint8_t *find_value(
    const uint8_t *response,
    size_t response_len,
    const char *value) {
    const size_t value_len = strlen(value);
    if (value_len == 0u || response_len < value_len) {
        return NULL;
    }
    for (size_t i = 0u; i + value_len <= response_len; ++i) {
        if (memcmp(&response[i], value, value_len) == 0) {
            return &response[i];
        }
    }
    return NULL;
}

static int contains_terminal_token(
    const uint8_t *line,
    size_t line_len,
    const char *token) {
    const size_t token_len = strlen(token);

    if (token_len == 0u || line_len < token_len) {
        return 0;
    }
    for (size_t i = 0u; i + token_len <= line_len; ++i) {
        const size_t end = i + token_len;
        if (memcmp(&line[i], token, token_len) == 0 &&
            (i == 0u || line[i - 1u] == ' ') &&
            (end == line_len || line[end] == ' ')) {
            return 1;
        }
    }
    return 0;
}

h2_h2loader_host_command_terminal_t h2_h2loader_host_command_parse_terminal(
    const uint8_t *response,
    size_t response_len,
    const h2_h2loader_host_command_contract_t *contract) {
    const uint8_t *line;
    size_t line_len;

    if (response == NULL || contract == NULL || contract->marker == NULL) {
        return H2_H2LOADER_HOST_COMMAND_TERMINAL_NONE;
    }
    line = find_value(response, response_len, contract->marker);
    if (line == NULL) {
        return H2_H2LOADER_HOST_COMMAND_TERMINAL_NONE;
    }
    line_len = response_len - (size_t)(line - response);
    for (size_t i = 0u; i < line_len; ++i) {
        if (line[i] == '\r' || line[i] == '\n') {
            line_len = i;
            break;
        }
    }
    if (contains(line, line_len, "result=unsupported") ||
        contains(line, line_len, "result=unavailable")) {
        return H2_H2LOADER_HOST_COMMAND_TERMINAL_UNSUPPORTED;
    }
    if (contains(line, line_len, "usage:") ||
        contains(line, line_len, "result=invalid_command")) {
        return H2_H2LOADER_HOST_COMMAND_TERMINAL_USAGE;
    }
    if (contains(line, line_len, "result=error") ||
        contains(line, line_len, "result=fail")) {
        return H2_H2LOADER_HOST_COMMAND_TERMINAL_ERROR;
    }
    if (contract->marker_is_success != 0u) {
        return H2_H2LOADER_HOST_COMMAND_TERMINAL_OK;
    }
    if (contract->success_token != NULL &&
        contains_terminal_token(
            line, line_len, contract->success_token)) {
        return H2_H2LOADER_HOST_COMMAND_TERMINAL_OK;
    }
    return H2_H2LOADER_HOST_COMMAND_TERMINAL_NONE;
}

h2_pal_result_t h2_h2loader_host_command_execute_transport_with_finish(
    void *transport,
    h2_h2loader_host_command_write_fn write_command,
    h2_h2loader_host_command_read_fn read_response,
    h2_h2loader_host_command_finish_fn finish_response,
    const h2_h2loader_host_command_request_t *request,
    h2_h2loader_host_command_result_t *out_result) {
    uint8_t response[8192u];
    h2_h2loader_host_command_contract_t contract;
    size_t response_len = 0u;
    h2_pal_result_t rc;

    if (out_result != NULL) {
        memset(out_result, 0, sizeof(*out_result));
    }
    if (transport == NULL || write_command == NULL ||
        read_response == NULL || request == NULL || out_result == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_h2loader_host_command_contract(request, &contract);
    if (rc != H2_PAL_OK) {
        out_result->transport_result = rc;
        return rc;
    }
    out_result->lifecycle_transition = contract.lifecycle_transition;
    if (request->is_cancelled != NULL &&
        request->is_cancelled(request->cancel_user)) {
        out_result->transport_result = H2_PAL_EXIT;
        return H2_PAL_EXIT;
    }
    rc = write_command(transport, contract.line);
    if (rc != H2_PAL_OK) {
        out_result->transport_result = rc;
        return rc;
    }
    rc = read_response(
        transport,
        contract.marker,
        response,
        sizeof(response),
        &response_len,
        request->on_output,
        request->output_user);
    out_result->output_bytes = response_len;
    out_result->output_truncated = rc == H2_PAL_ERR_NO_SPACE ? 1u : 0u;
    if (request->is_cancelled != NULL &&
        request->is_cancelled(request->cancel_user)) {
        out_result->transport_result = H2_PAL_EXIT;
        return H2_PAL_EXIT;
    }
    out_result->transport_result = rc;
    if (rc != H2_PAL_OK) {
        /* Reboot-class commands acknowledge and then reboot, dropping the
         * link. Depending on the transport this surfaces as a disconnect
         * (CLOSED, e.g. USB CDC removal) or as a read timeout (the browser
         * simply stops receiving bytes). Once the acknowledged marker is in
         * the buffer the command was accepted, so resolve it as OK either
         * way; a timeout without the marker is still a genuine failure. */
        if ((rc == H2_PAL_ERR_CLOSED || rc == H2_PAL_ERR_TIMEOUT) &&
            contract.accepted_disconnect_token != NULL &&
            contains(
                response,
                response_len,
                contract.accepted_disconnect_token)) {
            out_result->terminal = H2_H2LOADER_HOST_COMMAND_TERMINAL_OK;
            if (finish_response == NULL) {
                return H2_PAL_OK;
            }
            rc = finish_response(transport);
            out_result->transport_result = rc;
            return rc;
        }
        return rc;
    }
    out_result->terminal = h2_h2loader_host_command_parse_terminal(
        response, response_len, &contract);
    if (out_result->terminal == H2_H2LOADER_HOST_COMMAND_TERMINAL_OK) {
        if (contract.lifecycle_transition == 0u || finish_response == NULL) {
            return H2_PAL_OK;
        }
        rc = finish_response(transport);
        out_result->transport_result = rc;
        return rc;
    }
    return out_result->terminal ==
            H2_H2LOADER_HOST_COMMAND_TERMINAL_UNSUPPORTED
        ? H2_PAL_ERR_UNSUPPORTED
        : H2_PAL_ERR_IO;
}

h2_pal_result_t h2_h2loader_host_command_execute_transport(
    void *transport,
    h2_h2loader_host_command_write_fn write_command,
    h2_h2loader_host_command_read_fn read_response,
    const h2_h2loader_host_command_request_t *request,
    h2_h2loader_host_command_result_t *out_result) {
    return h2_h2loader_host_command_execute_transport_with_finish(
        transport,
        write_command,
        read_response,
        NULL,
        request,
        out_result);
}
