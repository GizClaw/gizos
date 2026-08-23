#include "h2_h2loader_cli_internal.h"

#include "h2_bleikcp.h"

#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPEED_HEADER_SIZE 24u
#define SPEED_CHUNK_SIZE 4096u
#define SPEED_SETUP_TIMEOUT_MS 10000u

static const uint8_t speed_service_uuid[] = {0xe0u, 0xfeu};
static const uint8_t speed_tx_uuid[] = {0xe1u, 0xfeu};
static const uint8_t speed_rx_uuid[] = {0xe2u, 0xfeu};

typedef struct speed_scan {
    atomic_bool owned;
    atomic_flag guard;
    h2_pal_ble_addr_t address;
    int rssi;
    int found;
} speed_scan_t;

static speed_scan_t speed_scan = {.guard = ATOMIC_FLAG_INIT};

static void speed_scan_lock(speed_scan_t *scan) {
    while (atomic_flag_test_and_set_explicit(&scan->guard, memory_order_acquire)) {}
}

static void speed_scan_unlock(speed_scan_t *scan) {
    atomic_flag_clear_explicit(&scan->guard, memory_order_release);
}

typedef struct speed_result {
    uint64_t session;
    uint64_t elapsed_ms;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    uint16_t mtu;
    int rssi;
    unsigned reconnects;
    h2_bleikcp_stats_t transport;
    h2_pal_result_t result;
} speed_result_t;

static int parse_positive_double(const char *value, double *out) {
    char *end = NULL;
    errno = 0;
    *out = strtod(value, &end);
    return errno == 0 && end != value && *end == '\0' &&
        *out > 0.0 && *out <= 3600.0;
}

static int parse_runs(const char *value, unsigned *out) {
    char *end = NULL;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed == 0u || parsed > 32u) return 0;
    *out = (unsigned)parsed;
    return 1;
}

static bool speed_scan_result(
    void *user,
    const h2_pal_ble_scan_result_t *result) {
    speed_scan_t *scan = user;
    int has_service = 0;
    for (size_t i = 0u; i < result->service_uuid_count; ++i) {
        if (result->service_uuids[i].len == sizeof(speed_service_uuid) &&
            memcmp(result->service_uuids[i].data, speed_service_uuid,
                sizeof(speed_service_uuid)) == 0) {
            has_service = 1;
            break;
        }
    }
    if (!result->connectable ||
        result->data_status != H2_PAL_BLE_ADV_DATA_COMPLETE || !has_service) {
        return false;
    }
    speed_scan_lock(scan);
    if (atomic_load_explicit(&scan->owned, memory_order_relaxed) &&
        (!scan->found || result->rssi > scan->rssi)) {
        scan->address = result->addr;
        scan->rssi = result->rssi;
        scan->found = 1;
    }
    speed_scan_unlock(scan);
    return false;
}

static uint64_t read_u64(const uint8_t *data) {
    uint64_t value = 0u;
    for (unsigned i = 0u; i < 8u; ++i) value |= (uint64_t)data[i] << (i * 8u);
    return value;
}

static uint32_t read_u32(const uint8_t *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
        ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static void write_u32(uint8_t *out, uint32_t value) {
    for (unsigned i = 0u; i < 4u; ++i) out[i] = (uint8_t)(value >> (i * 8u));
}

static void write_u64(uint8_t *out, uint64_t value) {
    for (unsigned i = 0u; i < 8u; ++i) out[i] = (uint8_t)(value >> (i * 8u));
}

static void make_header(uint8_t out[SPEED_HEADER_SIZE], uint64_t session) {
    memset(out, 0, SPEED_HEADER_SIZE);
    memcpy(out, "H2BS", 4u);
    out[4] = 1u;
    write_u64(&out[8], session);
    write_u32(&out[16], SPEED_CHUNK_SIZE);
}

static int validate_response(
    const uint8_t header[SPEED_HEADER_SIZE],
    uint64_t session) {
    return memcmp(header, "H2BR", 4u) == 0 && header[4] == 1u &&
        header[5] == 0u && header[6] == 0u && header[7] == 0u &&
        read_u64(&header[8]) == session &&
        read_u32(&header[16]) == SPEED_CHUNK_SIZE &&
        read_u32(&header[20]) == 0u;
}

static uint8_t payload_byte(
    uint64_t session,
    uint8_t direction,
    uint64_t offset) {
    return (uint8_t)((session >> ((offset & 7u) * 8u)) ^
        ((uint64_t)direction * 0x5au) ^ offset);
}

static void fill_payload(
    uint8_t *out,
    size_t len,
    uint64_t session,
    uint8_t direction,
    uint64_t offset) {
    for (size_t i = 0u; i < len; ++i) {
        out[i] = payload_byte(session, direction, offset + i);
    }
}

static int verify_payload(
    const uint8_t *data,
    size_t len,
    uint64_t session,
    uint8_t direction,
    uint64_t offset) {
    for (size_t i = 0u; i < len; ++i) {
        if (data[i] != payload_byte(session, direction, offset + i)) return 0;
    }
    return 1;
}

h2_pal_result_t h2_h2loader_cli_find_ble_peer(
    h2_h2loader_cli_context_t *context,
    uint32_t timeout_ms,
    h2_pal_ble_addr_t *out_address,
    int *out_rssi) {
    speed_scan_t *scan = &speed_scan;
    bool expected = false;
    int scan_started = 0;
    int scan_quiesced = 1;
    h2_pal_result_t rc;
    if (!atomic_compare_exchange_strong_explicit(
            &scan->owned,
            &expected,
            true,
            memory_order_acq_rel,
            memory_order_acquire)) {
        return H2_PAL_ERR_BUSY;
    }
    speed_scan_lock(scan);
    memset(&scan->address, 0, sizeof(scan->address));
    scan->rssi = 0;
    scan->found = 0;
    speed_scan_unlock(scan);
    h2_pal_ble_scan_params_t params = {
        .mode = H2_PAL_BLE_SCAN_MODE_ACTIVE,
        .interval_ms = 50u,
        .window_ms = 50u,
        .timeout_ms = timeout_ms,
        .type = H2_PAL_BLE_SCAN_TYPE_LEGACY,
        .phy_mask = H2_PAL_BLE_SCAN_PHY_1M,
    };
    rc = h2_pal_ble_start_scan(
        context->runtime->ble_host, &params, speed_scan_result, scan);
    scan_started = rc == H2_PAL_OK;
    scan_quiesced = !scan_started;
    for (uint32_t waited = 0u; rc == H2_PAL_OK && waited < timeout_ms;) {
        uint32_t step = timeout_ms - waited > 50u ? 50u : timeout_ms - waited;
        int found;
        if (context->config->is_cancelled != NULL &&
            context->config->is_cancelled(context->config->cancel_user)) {
            rc = H2_PAL_EXIT;
            break;
        }
        rc = h2_pal_time_sleep_ms(context->runtime->time, step);
        waited += step;
        speed_scan_lock(scan);
        found = scan->found;
        speed_scan_unlock(scan);
        if (found) break;
    }
    if (scan_started) {
        h2_pal_result_t stop_rc = H2_PAL_ERR_IO;
        for (unsigned attempt = 0u; attempt < 3u; ++attempt) {
            stop_rc = h2_pal_ble_stop_scan(context->runtime->ble_host);
            if (stop_rc == H2_PAL_OK || stop_rc == H2_PAL_ERR_INVALID_STATE) {
                scan_quiesced = 1;
                break;
            }
        }
        if (!scan_quiesced) {
            for (unsigned attempt = 0u; attempt < 3u; ++attempt) {
                h2_pal_result_t host_stop_rc = h2_pal_ble_stop(
                    context->runtime->ble_host);
                if (host_stop_rc == H2_PAL_OK ||
                    host_stop_rc == H2_PAL_ERR_INVALID_STATE) {
                    scan_quiesced = 1;
                    break;
                }
            }
        }
        if (rc == H2_PAL_OK && stop_rc != H2_PAL_OK &&
            stop_rc != H2_PAL_ERR_INVALID_STATE) {
            rc = stop_rc;
        }
    }
    if (rc == H2_PAL_OK) {
        speed_scan_lock(scan);
        if (scan->found) {
            *out_address = scan->address;
            *out_rssi = scan->rssi;
        }
        else rc = H2_PAL_ERR_NOT_FOUND;
        speed_scan_unlock(scan);
    }
    if (scan_quiesced) {
        atomic_store_explicit(&scan->owned, false, memory_order_release);
    }
    return rc;
}

static h2_bleikcp_config_t stream_config(void) {
    return (h2_bleikcp_config_t){
        .service_uuid = {speed_service_uuid, sizeof(speed_service_uuid)},
        .tx_char_uuid = {speed_tx_uuid, sizeof(speed_tx_uuid)},
        .rx_char_uuid = {speed_rx_uuid, sizeof(speed_rx_uuid)},
        .send_window = 32u,
        .recv_window = 32u,
        .input_frame_capacity = 64u,
        .tx_buffer_size = 32u * 1024u,
        .rx_buffer_size = 32u * 1024u,
        .output_retry_count = 40u,
        .output_retry_delay_ms = 2u,
        .setup_timeout_ms = SPEED_SETUP_TIMEOUT_MS,
        .worker_task_options = {
            .name = "h2loader-cli-speed",
            .min_stack_size = 12u * 1024u,
        },
    };
}

static h2_pal_result_t run_once(
    h2_h2loader_cli_context_t *context,
    uint32_t duration_ms,
    uint32_t scan_timeout_ms,
    speed_result_t *out) {
    h2_pal_ble_addr_t address;
    uint16_t connection = H2_PAL_BLE_INVALID_CONN_HANDLE;
    uint16_t mtu = 0u;
    h2_bleikcp_t *stream = NULL;
    uint8_t header[SPEED_HEADER_SIZE];
    uint8_t payload[SPEED_CHUNK_SIZE];
    size_t read_len = 0u;
    uint64_t start = 0u;
    uint64_t now = 0u;
    uint64_t tx_offset = 0u;
    uint64_t rx_offset = 0u;
    uint64_t session = 0u;
    size_t header_read = 0u;
    int rssi = -127;
    h2_pal_result_t rc = h2_h2loader_cli_find_ble_peer(
        context, scan_timeout_ms, &address, &rssi);
    if (rc != H2_PAL_OK) goto cleanup;
    h2_pal_ble_connect_params_t connect = {
        .timeout_ms = SPEED_SETUP_TIMEOUT_MS,
        .interval_min_ms = 15u,
        .interval_max_ms = 15u,
        .supervision_timeout_ms = 4000u,
    };
    rc = h2_pal_ble_connect(
        context->runtime->ble_host, &address, &connect, &connection);
    if (rc == H2_PAL_OK) {
        rc = h2_pal_ble_exchange_mtu(
            context->runtime->ble_host, connection, &mtu,
            SPEED_SETUP_TIMEOUT_MS);
    }
    h2_bleikcp_api_t api = {
        .ble = context->runtime->ble_host,
        .task = context->runtime->task,
        .time = context->runtime->time,
        .sync = context->runtime->sync,
        .system_event = context->runtime->system_event,
        .allocator = context->runtime->mem,
    };
    h2_bleikcp_config_t config = stream_config();
    if (rc == H2_PAL_OK) {
        rc = h2_bleikcp_client_open(&api, &config, connection, mtu, &stream);
    }
    if (rc == H2_PAL_OK) {
        rc = h2_pal_time_get_monotonic_ms(context->runtime->time, &start);
    }
    session = start ^ ((uint64_t)connection << 48u);
    if (session == 0u) session = 1u;
    make_header(header, session);
    if (rc == H2_PAL_OK) {
        rc = h2_bleikcp_write(
            stream, header, sizeof(header), SPEED_SETUP_TIMEOUT_MS);
    }
    if (rc == H2_PAL_OK) rc = h2_bleikcp_flush(stream, SPEED_SETUP_TIMEOUT_MS);
    while (rc == H2_PAL_OK && header_read < sizeof(header)) {
        rc = h2_pal_time_get_monotonic_ms(context->runtime->time, &now);
        if (rc != H2_PAL_OK) break;
        if (now - start >= SPEED_SETUP_TIMEOUT_MS) {
            rc = H2_PAL_ERR_TIMEOUT;
            break;
        }
        uint64_t remaining = SPEED_SETUP_TIMEOUT_MS - (now - start);
        rc = h2_bleikcp_read(
            stream, &header[header_read], sizeof(header) - header_read,
            &read_len, (uint32_t)remaining);
        if (rc == H2_PAL_OK &&
            (read_len == 0u || read_len > sizeof(header) - header_read)) {
            rc = H2_PAL_ERR_IO;
        }
        if (rc == H2_PAL_OK) header_read += read_len;
    }
    if (rc == H2_PAL_OK &&
        !validate_response(header, session)) {
        rc = H2_PAL_ERR_FORMAT;
    }
    now = start;
    while (rc == H2_PAL_OK && now - start < duration_ms &&
           (context->config->is_cancelled == NULL ||
            !context->config->is_cancelled(context->config->cancel_user))) {
        fill_payload(payload, sizeof(payload), session, 0u, tx_offset);
        rc = h2_bleikcp_write(stream, payload, sizeof(payload), 5000u);
        if (rc == H2_PAL_OK) rc = h2_bleikcp_flush(stream, 5000u);
        if (rc == H2_PAL_OK) {
            tx_offset += sizeof(payload);
            rc = h2_bleikcp_read(
                stream, payload, sizeof(payload), &read_len, 5000u);
        }
        if (rc == H2_PAL_OK &&
            !verify_payload(payload, read_len, session, 1u, rx_offset)) {
            rc = H2_PAL_ERR_FORMAT;
        }
        if (rc == H2_PAL_OK) rx_offset += read_len;
        if (rc == H2_PAL_OK) {
            rc = h2_pal_time_get_monotonic_ms(context->runtime->time, &now);
        }
    }
    if (rc == H2_PAL_OK && context->config->is_cancelled != NULL &&
        context->config->is_cancelled(context->config->cancel_user)) {
        rc = H2_PAL_EXIT;
    }
cleanup:
    if (stream != NULL) {
        (void)h2_bleikcp_get_stats(stream, &out->transport);
    }
    if (stream != NULL) (void)h2_bleikcp_close(stream);
    if (connection != H2_PAL_BLE_INVALID_CONN_HANDLE) {
        (void)h2_pal_ble_disconnect(context->runtime->ble_host, connection);
    }
    out->session = session;
    out->elapsed_ms = now >= start ? now - start : 0u;
    out->tx_bytes = tx_offset;
    out->rx_bytes = rx_offset;
    out->mtu = mtu;
    out->rssi = rssi;
    out->result = rc;
    return rc;
}

static h2_pal_result_t write_json(
    h2_h2loader_cli_context_t *context,
    const char *path,
    const speed_result_t *results,
    unsigned runs,
    double requested_duration) {
    h2_pal_fs_file_t *file = NULL;
    char json[65536];
    size_t offset;
    h2_pal_result_t rc = H2_PAL_OK;
    if (context->runtime->fs == NULL || path == NULL || path[0] != '/') {
        return H2_PAL_ERR_INVALID_ARG;
    }
    rc = h2_pal_fs_open(
        context->runtime->fs, path, H2_PAL_FS_OPEN_WRITE_TRUNCATE, &file);
    if (rc != H2_PAL_OK) return rc;
    offset = (size_t)snprintf(json, sizeof(json),
        "{\n  \"schema_version\": 1,\n  \"client\": \"native-pal\",\n"
        "  \"duration_seconds\": %.3f,\n  \"requested_runs\": %u,\n"
        "  \"runs\": [\n",
        requested_duration, runs);
    for (unsigned run = 0u; run < runs && rc == H2_PAL_OK; ++run) {
        int wrote = snprintf(&json[offset], sizeof(json) - offset,
            "    %s{\"run\": %u, \"session_id\": \"%016llx\", "
            "\"peer\": {\"address\": null, \"name\": null, \"rssi\": %d}, "
            "\"elapsed_seconds\": %.3f, \"payload\": {\"tx_bytes\": %llu, "
            "\"rx_bytes\": %llu, \"rx_sha256\": null, \"mismatches\": 0, "
            "\"verified\": true}, \"throughput_kib_s\": {\"tx_average\": %.3f, "
            "\"rx_average\": %.3f, \"tx_5s\": null, \"rx_5s\": null}, "
            "\"link\": {\"effective_att_payload\": %u, \"att_mtu\": %u, "
            "\"phy\": \"unknown\", \"interval_ms\": 0}, "
            "\"transport\": {\"kcp_mtu\": %u, \"tx_frames\": %llu, "
            "\"rx_frames\": %llu, \"input_errors\": %llu, "
            "\"dropped_input\": %llu, \"output_blocked\": %llu, "
            "\"output_retries\": %llu, \"retransmits\": %llu, "
            "\"disconnects\": %llu, \"waitsnd\": %u}, "
            "\"reconnects\": %u, \"disconnect_reason\": null, \"error\": null}\n",
            run == 0u ? "" : ",", run + 1u,
            (unsigned long long)results[run].session, results[run].rssi,
            results[run].elapsed_ms / 1000.0,
            (unsigned long long)results[run].tx_bytes,
            (unsigned long long)results[run].rx_bytes,
            results[run].elapsed_ms == 0u ? 0.0 :
                results[run].tx_bytes * 1000.0 / 1024.0 /
                    results[run].elapsed_ms,
            results[run].elapsed_ms == 0u ? 0.0 :
                results[run].rx_bytes * 1000.0 / 1024.0 /
                    results[run].elapsed_ms,
            results[run].mtu >= 3u ? (unsigned)results[run].mtu - 3u : 0u,
            (unsigned)results[run].mtu,
            (unsigned)results[run].transport.kcp_mtu,
            (unsigned long long)results[run].transport.tx_frames,
            (unsigned long long)results[run].transport.rx_frames,
            (unsigned long long)results[run].transport.input_errors,
            (unsigned long long)results[run].transport.dropped_input,
            (unsigned long long)results[run].transport.output_blocked,
            (unsigned long long)results[run].transport.output_retries,
            (unsigned long long)results[run].transport.retransmits,
            (unsigned long long)results[run].transport.disconnects,
            (unsigned)results[run].transport.waitsnd,
            results[run].reconnects);
        if (wrote < 0 || (size_t)wrote >= sizeof(json) - offset) {
            rc = H2_PAL_ERR_NO_SPACE;
        } else {
            offset += (size_t)wrote;
        }
    }
    if (rc == H2_PAL_OK) {
        int wrote = snprintf(&json[offset], sizeof(json) - offset, "  ]\n}\n");
        if (wrote < 0 || (size_t)wrote >= sizeof(json) - offset) {
            rc = H2_PAL_ERR_NO_SPACE;
        } else {
            offset += (size_t)wrote;
        }
    }
    if (rc == H2_PAL_OK) {
        size_t written_total = 0u;
        while (written_total < offset && rc == H2_PAL_OK) {
            size_t written = 0u;
            rc = h2_pal_fs_write(context->runtime->fs, file,
                (const uint8_t *)&json[written_total], offset - written_total,
                &written);
            if (rc != H2_PAL_OK) break;
            if (written == 0u || written > offset - written_total) {
                rc = H2_PAL_ERR_IO;
            } else {
                written_total += written;
            }
        }
    }
    if (rc == H2_PAL_OK) rc = h2_pal_fs_sync(context->runtime->fs, file);
    h2_pal_result_t close_rc = h2_pal_fs_close(context->runtime->fs, file);
    if (rc == H2_PAL_OK) rc = close_rc;
    return rc;
}

int h2_h2loader_cli_bleikcp_speed_command(
    h2_h2loader_cli_context_t *context,
    int argc,
    const char *const *argv) {
    double duration = 60.0;
    double scan_timeout = 5.0;
    unsigned runs = 3u;
    const char *json_out = NULL;
    speed_result_t results[32] = {0};
    h2_pal_result_t rc = H2_PAL_OK;
    if (context->runtime->fs == NULL) {
        return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    for (int i = 0; i < argc; ++i) {
        if (i + 1 >= argc) return H2_H2LOADER_CLI_EXIT_USAGE;
        const char *value = argv[++i];
        if (strcmp(argv[i - 1], "--duration") == 0) {
            if (!parse_positive_double(value, &duration)) {
                return H2_H2LOADER_CLI_EXIT_USAGE;
            }
        } else if (strcmp(argv[i - 1], "--runs") == 0) {
            if (!parse_runs(value, &runs)) return H2_H2LOADER_CLI_EXIT_USAGE;
        } else if (strcmp(argv[i - 1], "--scan-timeout") == 0) {
            if (!parse_positive_double(value, &scan_timeout)) {
                return H2_H2LOADER_CLI_EXIT_USAGE;
            }
        } else if (strcmp(argv[i - 1], "--json-out") == 0) {
            json_out = value;
        } else {
            return H2_H2LOADER_CLI_EXIT_USAGE;
        }
    }
    if (json_out == NULL) return H2_H2LOADER_CLI_EXIT_USAGE;
    if (h2_h2loader_cli_acquire_ble(context) == NULL) {
        h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDERR,
            "h2loader: bleikcp-speed unsupported: no real BLE Host PAL\n");
        return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    for (unsigned run = 0u; run < runs; ++run) {
        for (unsigned attempt = 0u; attempt < 3u; ++attempt) {
            memset(&results[run], 0, sizeof(results[run]));
            results[run].reconnects = attempt;
            rc = run_once(context, (uint32_t)(duration * 1000.0),
                (uint32_t)(scan_timeout * 1000.0), &results[run]);
            if (rc == H2_PAL_OK || rc == H2_PAL_EXIT ||
                rc == H2_PAL_ERR_FORMAT) break;
            if (context->config->is_cancelled != NULL &&
                context->config->is_cancelled(context->config->cancel_user)) {
                rc = H2_PAL_EXIT;
                break;
            }
            (void)h2_pal_time_sleep_ms(context->runtime->time, 500u);
        }
        h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDOUT,
            "H2_BLEIKCP_SPEED_HOST run=%u elapsed=%.3f tx_bytes=%llu "
            "rx_bytes=%llu tx_kib_s=%.1f rx_kib_s=%.1f mtu=%u "
            "mismatches=%u\n",
            run + 1u, results[run].elapsed_ms / 1000.0,
            (unsigned long long)results[run].tx_bytes,
            (unsigned long long)results[run].rx_bytes,
            results[run].elapsed_ms == 0u ? 0.0 :
                results[run].tx_bytes * 1000.0 / 1024.0 /
                    results[run].elapsed_ms,
            results[run].elapsed_ms == 0u ? 0.0 :
                results[run].rx_bytes * 1000.0 / 1024.0 /
                    results[run].elapsed_ms,
            (unsigned)results[run].mtu,
            results[run].result == H2_PAL_ERR_FORMAT ? 1u : 0u);
        if (rc != H2_PAL_OK) return H2_H2LOADER_CLI_EXIT_RUNTIME;
    }
    rc = write_json(context, json_out, results, runs, duration);
    if (rc != H2_PAL_OK) return H2_H2LOADER_CLI_EXIT_RUNTIME;
    h2_h2loader_cli_output(context, H2_H2LOADER_CLI_STREAM_STDOUT,
        "H2_BLEIKCP_SPEED_RESULT path=%s\n", json_out);
    return H2_H2LOADER_CLI_EXIT_OK;
}
