#include "h2_h2loader_host.h"

#include "h2_h2loader_host_internal.h"

#include <stdio.h>
#include <string.h>

static const uint8_t h2_h2loader_host_ble_service_uuid[16] = {
    0x1du, 0x72u, 0xa1u, 0x6bu, 0x3au, 0xafu, 0x0bu, 0xaau,
    0xe2u, 0x53u, 0xd8u, 0x3eu, 0x70u, 0xb5u, 0xa4u, 0x71u,
};

typedef struct h2_h2loader_host_scan_context {
    const h2_h2loader_host_scan_config_t *config;
    h2_pal_mutex_t *mutex;
    size_t count;
    size_t required_capacity;
    h2_pal_ble_addr_t ble_seen[128];
    size_t ble_seen_count;
} h2_h2loader_host_scan_context_t;

static uint32_t read_le32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
        ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static uint64_t read_le64(const uint8_t *data) {
    uint64_t value = 0u;
    for (size_t i = 0u; i < 8u; ++i) {
        value |= (uint64_t)data[i] << (i * 8u);
    }
    return value;
}

static int scan_contains_candidate(
    const h2_h2loader_host_scan_context_t *context,
    const char *candidate_id) {
    size_t stored = context->count < context->config->candidate_capacity
        ? context->count
        : context->config->candidate_capacity;
    for (size_t i = 0u; i < stored; ++i) {
        if (strcmp(
                context->config->candidates[i].candidate_id,
                candidate_id) == 0) {
            return 1;
        }
    }
    return 0;
}

static void scan_add_candidate(
    h2_h2loader_host_scan_context_t *context,
    const h2_h2loader_host_candidate_t *candidate) {
    if (context->mutex != NULL) {
        (void)h2_pal_mutex_lock(context->config->sync, context->mutex);
    }
    if (!scan_contains_candidate(context, candidate->candidate_id)) {
        if (context->count < context->config->candidate_capacity) {
            context->config->candidates[context->count] = *candidate;
        }
        ++context->count;
        if (context->count > context->required_capacity) {
            context->required_capacity = context->count;
        }
    }
    if (context->mutex != NULL) {
        (void)h2_pal_mutex_unlock(context->config->sync, context->mutex);
    }
}

static int scan_result_has_service(
    const h2_pal_ble_scan_result_t *result) {
    for (size_t i = 0u; i < result->service_uuid_count; ++i) {
        const h2_pal_ble_uuid_t *uuid = &result->service_uuids[i];
        if (uuid->len == sizeof(h2_h2loader_host_ble_service_uuid) &&
            memcmp(
                uuid->data,
                h2_h2loader_host_ble_service_uuid,
                sizeof(h2_h2loader_host_ble_service_uuid)) == 0) {
            return 1;
        }
    }
    return 0;
}

static int parse_ble_identity(
    const h2_pal_ble_scan_result_t *result,
    h2_h2loader_host_candidate_t *out_candidate) {
    const uint8_t *payload = result->service_data.len > 0u
        ? result->service_data.data
        : result->manufacturer_data.data;
    size_t payload_len = result->service_data.len > 0u
        ? result->service_data.len
        : result->manufacturer_data.len;
    uint8_t version;
    uint32_t capabilities;

    if (!scan_result_has_service(result) ||
        result->data_status != H2_PAL_BLE_ADV_DATA_COMPLETE ||
        !result->connectable || payload == NULL || payload_len < 10u ||
        memcmp(payload, "H2LD", 4u) != 0) {
        return 0;
    }
    version = payload[4];
    capabilities = read_le32(&payload[6]);
    if ((version != 1u && version != 2u) ||
        payload[5] != 0u ||
        (capabilities & ~H2_H2LOADER_HOST_CAPABILITIES_ALL) != 0u) {
        return 0;
    }
    if (version == 1u) {
        size_t board_len;
        if (payload_len < 11u) {
            return 0;
        }
        board_len = payload[10];
        if (board_len == 0u ||
            board_len >= sizeof(out_candidate->advertised_board) ||
            payload_len != 11u + board_len ||
            !h2_h2loader_host_copy_text(
                out_candidate->advertised_board,
                sizeof(out_candidate->advertised_board),
                (const char *)&payload[11],
                board_len) ||
            !h2_h2loader_host_is_safe_identity(
                out_candidate->advertised_board)) {
            return 0;
        }
    } else {
        if (payload_len != 18u) {
            return 0;
        }
        int length = snprintf(
            out_candidate->advertised_board,
            sizeof(out_candidate->advertised_board),
            "fnv1a64:%016llx",
            (unsigned long long)read_le64(&payload[10]));
        if (length <= 0 ||
            (size_t)length >= sizeof(out_candidate->advertised_board)) {
            return 0;
        }
    }
    out_candidate->advertised_capabilities = capabilities;
    return 1;
}

static bool scan_ble_callback(
    void *user,
    const h2_pal_ble_scan_result_t *result) {
    h2_h2loader_host_scan_context_t *context = user;
    h2_h2loader_host_candidate_t candidate;
    int length;

    if (context == NULL || result == NULL) {
        return false;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.transport = H2_H2LOADER_HOST_TRANSPORT_BLE;
    candidate.ble_address = result->addr;
    candidate.rssi = result->rssi;
    if (!parse_ble_identity(result, &candidate)) {
        return false;
    }
    (void)h2_pal_mutex_lock(context->config->sync, context->mutex);
    for (size_t i = 0u; i < context->ble_seen_count; ++i) {
        if (context->ble_seen[i].type == result->addr.type &&
            memcmp(
                context->ble_seen[i].value,
                result->addr.value,
                H2_PAL_BLE_ADDR_LEN) == 0) {
            (void)h2_pal_mutex_unlock(
                context->config->sync, context->mutex);
            return false;
        }
    }
    if (context->ble_seen_count >=
        sizeof(context->ble_seen) / sizeof(context->ble_seen[0])) {
        (void)h2_pal_mutex_unlock(context->config->sync, context->mutex);
        return true;
    }
    context->ble_seen[context->ble_seen_count++] = result->addr;
    (void)h2_pal_mutex_unlock(context->config->sync, context->mutex);
    length = snprintf(
        candidate.candidate_id,
        sizeof(candidate.candidate_id),
        "ble:%u:%02x%02x%02x%02x%02x%02x",
        (unsigned)result->addr.type,
        result->addr.value[0],
        result->addr.value[1],
        result->addr.value[2],
        result->addr.value[3],
        result->addr.value[4],
        result->addr.value[5]);
    if (length <= 0 ||
        (size_t)length >= sizeof(candidate.candidate_id)) {
        return false;
    }
    (void)h2_h2loader_host_copy_text(
        candidate.endpoint,
        sizeof(candidate.endpoint),
        candidate.candidate_id + 4u,
        strlen(candidate.candidate_id + 4u));
    length = snprintf(
        candidate.display_name,
        sizeof(candidate.display_name),
        "h2l.%s",
        strncmp(candidate.advertised_board, "fnv1a64:", 8u) == 0
            ? "unknown"
            : candidate.advertised_board);
    if (length <= 0 ||
        (size_t)length >= sizeof(candidate.display_name)) {
        return false;
    }
    scan_add_candidate(context, &candidate);
    return false;
}

static h2_pal_result_t scan_serial(
    h2_h2loader_host_scan_context_t *context) {
    h2_pal_serial_host_snapshot_t *snapshot = NULL;
    size_t count = 0u;
    h2_pal_result_t rc =
        h2_pal_serial_host_scan(context->config->serial, &snapshot);

    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_pal_serial_host_snapshot_count(
        context->config->serial, snapshot, &count);
    for (size_t i = 0u; rc == H2_PAL_OK && i < count; ++i) {
        h2_pal_serial_host_port_info_t info;
        h2_h2loader_host_candidate_t candidate;
        int length;

        rc = h2_pal_serial_host_snapshot_get(
            context->config->serial, snapshot, i, &info);
        if (rc != H2_PAL_OK) {
            break;
        }
        memset(&candidate, 0, sizeof(candidate));
        candidate.transport = H2_H2LOADER_HOST_TRANSPORT_SERIAL;
        length = snprintf(
            candidate.candidate_id,
            sizeof(candidate.candidate_id),
            "serial:%s",
            info.port_id);
        if (length <= 0 ||
            (size_t)length >= sizeof(candidate.candidate_id) ||
            !h2_h2loader_host_copy_text(
                candidate.port_id,
                sizeof(candidate.port_id),
                info.port_id,
                strlen(info.port_id)) ||
            !h2_h2loader_host_copy_text(
                candidate.endpoint,
                sizeof(candidate.endpoint),
                info.endpoint,
                strlen(info.endpoint))) {
            rc = H2_PAL_ERR_NO_SPACE;
            break;
        }
        const char *display =
            (info.valid_fields &
             H2_PAL_SERIAL_HOST_PORT_FIELD_DISPLAY_NAME) != 0u
            ? info.display_name
            : info.endpoint;
        if (!h2_h2loader_host_copy_text(
                candidate.display_name,
                sizeof(candidate.display_name),
                display,
                strlen(display))) {
            rc = H2_PAL_ERR_NO_SPACE;
            break;
        }
        candidate.serial_valid_fields = info.valid_fields;
        candidate.serial_capabilities = info.capabilities;
        candidate.usb_vid = info.usb_vid;
        candidate.usb_pid = info.usb_pid;
        if ((info.valid_fields &
             H2_PAL_SERIAL_HOST_PORT_FIELD_USB_SERIAL) != 0u) {
            candidate.usb_identity_valid = 1u;
            if (!h2_h2loader_host_copy_text(
                    candidate.usb_serial,
                    sizeof(candidate.usb_serial),
                    info.usb_serial,
                    strlen(info.usb_serial))) {
                rc = H2_PAL_ERR_NO_SPACE;
                break;
            }
        }
        scan_add_candidate(context, &candidate);
    }
    h2_pal_result_t destroy_rc =
        h2_pal_serial_host_snapshot_destroy(
            context->config->serial, &snapshot);
    return rc == H2_PAL_OK ? destroy_rc : rc;
}

h2_pal_result_t h2_h2loader_host_scan(
    const h2_h2loader_host_scan_config_t *config,
    h2_h2loader_host_scan_result_t *out_result) {
    h2_h2loader_host_scan_context_t context;
    h2_pal_result_t rc;
    int ble_started = 0;

    if (out_result != NULL) {
        memset(out_result, 0, sizeof(*out_result));
        out_result->serial_result = H2_PAL_ERR_UNSUPPORTED;
        out_result->ble_result = H2_PAL_ERR_UNSUPPORTED;
    }
    if (config == NULL || out_result == NULL ||
        (config->candidate_capacity > 0u && config->candidates == NULL) ||
        (config->ble != NULL &&
         (config->sync == NULL || config->time == NULL ||
          config->ble_timeout_ms == 0u))) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(&context, 0, sizeof(context));
    context.config = config;
    if (config->sync == NULL) {
        if (config->ble != NULL) {
            return H2_PAL_ERR_INVALID_ARG;
        }
    } else {
        const h2_pal_mutex_config_t mutex_config = {
            .name = "h2loader-host-scan",
            .allocator = NULL,
            .flags = H2_PAL_MUTEX_FLAG_NONE,
        };
        rc = h2_pal_mutex_create(
            config->sync, &mutex_config, &context.mutex);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    if (config->ble != NULL) {
        const h2_pal_ble_scan_params_t params = {
            .mode = H2_PAL_BLE_SCAN_MODE_ACTIVE,
            .interval_ms = 100u,
            .window_ms = 100u,
            .timeout_ms = config->ble_timeout_ms,
            .type = H2_PAL_BLE_SCAN_TYPE_LEGACY,
            .phy_mask = H2_PAL_BLE_SCAN_PHY_1M,
        };
        out_result->ble_result = h2_pal_ble_start_scan(
            config->ble, &params, scan_ble_callback, &context);
        ble_started = out_result->ble_result == H2_PAL_OK;
    }
    if (config->serial != NULL) {
        out_result->serial_result = scan_serial(&context);
    }
    if (ble_started) {
        h2_pal_result_t sleep_rc = h2_pal_time_sleep_ms(
            config->time, config->ble_timeout_ms);
        h2_pal_result_t stop_rc = h2_pal_ble_stop_scan(config->ble);
        out_result->ble_result =
            sleep_rc == H2_PAL_OK ? stop_rc : sleep_rc;
    }
    if (context.mutex != NULL) {
        (void)h2_pal_mutex_lock(config->sync, context.mutex);
    }
    out_result->count = context.count < config->candidate_capacity
        ? context.count
        : config->candidate_capacity;
    out_result->required_capacity = context.required_capacity;
    if (context.mutex != NULL) {
        (void)h2_pal_mutex_unlock(config->sync, context.mutex);
        (void)h2_pal_mutex_destroy(config->sync, context.mutex);
    }
    if (context.required_capacity > config->candidate_capacity) {
        return H2_PAL_ERR_NO_SPACE;
    }
    if (config->serial != NULL &&
        out_result->serial_result != H2_PAL_OK &&
        config->ble != NULL && out_result->ble_result != H2_PAL_OK) {
        return H2_PAL_ERR_UNAVAILABLE;
    }
    return H2_PAL_OK;
}
