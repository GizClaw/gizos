#include "h2_iperf_internal.h"

#include <stdio.h>
#include <string.h>

/* ---- time, random, log --------------------------------------------------- */

uint64_t h2_iperf_now_us(const h2_iperf_config_t *config) {
    uint64_t us = 0u;
    if (h2_pal_time_get_monotonic_us(config->time, &us) == H2_PAL_OK) {
        return us;
    }
    uint64_t ms = 0u;
    (void)h2_pal_time_get_monotonic_ms(config->time, &ms);
    return ms * 1000u;
}

uint64_t h2_iperf_now_ms(const h2_iperf_config_t *config) {
    uint64_t ms = 0u;
    if (h2_pal_time_get_monotonic_ms(config->time, &ms) == H2_PAL_OK) {
        return ms;
    }
    return h2_iperf_now_us(config) / 1000u;
}

void h2_iperf_sleep_ms(const h2_iperf_config_t *config, uint32_t ms) {
    if (h2_pal_time_sleep_ms(config->time, ms) != H2_PAL_OK) {
        /* Busy-wait fallback keeps pacing correct on providers without sleep. */
        uint64_t deadline = h2_iperf_now_ms(config) + ms;
        while (!h2_pal_time_deadline_expired(h2_iperf_now_ms(config), deadline)) {
        }
    }
}

static uint32_t xorshift32(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    *state = value == 0u ? 0x6d2b79f5u : value;
    return *state;
}

uint32_t h2_iperf_random_seed(const h2_iperf_config_t *config) {
    uint32_t seed = 0u;
    if (config->crypto != NULL &&
        h2_pal_crypto_random(config->crypto, (uint8_t *)&seed, sizeof(seed)) ==
            H2_PAL_OK &&
        seed != 0u) {
        return seed;
    }
    uint64_t now = h2_iperf_now_us(config);
    seed = (uint32_t)(now ^ (now >> 32u)) ^ 0x9e3779b9u;
    return seed == 0u ? 0x6d2b79f5u : seed;
}

void h2_iperf_random_fill(
    const h2_iperf_config_t *config,
    uint32_t *state,
    uint8_t *out,
    size_t len) {
    if (config->crypto != NULL &&
        h2_pal_crypto_random(config->crypto, out, len) == H2_PAL_OK) {
        return;
    }
    size_t index = 0u;
    while (index < len) {
        uint32_t word = xorshift32(state);
        for (unsigned byte = 0u; byte < 4u && index < len; ++byte) {
            out[index++] = (uint8_t)(word >> (byte * 8u));
        }
    }
}

void h2_iperf_make_cookie(
    const h2_iperf_config_t *config,
    uint32_t *state,
    char cookie[H2_IPERF_COOKIE_SIZE]) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
    uint8_t raw[H2_IPERF_COOKIE_SIZE];
    h2_iperf_random_fill(config, state, raw, sizeof(raw));
    for (size_t i = 0u; i + 1u < H2_IPERF_COOKIE_SIZE; ++i) {
        cookie[i] = alphabet[raw[i] % (sizeof(alphabet) - 1u)];
    }
    cookie[H2_IPERF_COOKIE_SIZE - 1u] = '\0';
}

void h2_iperf_log(
    const h2_iperf_config_t *config,
    h2_pal_log_level_t level,
    const char *format,
    ...) {
    if (config == NULL || config->log == NULL) {
        return;
    }
    char message[H2_PAL_LOG_MESSAGE_MAX];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    (void)h2_pal_log_write(config->log, level, "iperf", message);
}

bool h2_iperf_config_is_valid(const h2_iperf_config_t *config) {
    return config != NULL && config->mem != NULL && config->net != NULL &&
           config->time != NULL;
}

void h2_iperf_write_u32_be(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24u);
    out[1] = (uint8_t)(value >> 16u);
    out[2] = (uint8_t)(value >> 8u);
    out[3] = (uint8_t)value;
}

void h2_iperf_write_u32_le(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8u);
    out[2] = (uint8_t)(value >> 16u);
    out[3] = (uint8_t)(value >> 24u);
}

uint32_t h2_iperf_read_u32_be(const uint8_t *in) {
    return ((uint32_t)in[0] << 24u) | ((uint32_t)in[1] << 16u) |
           ((uint32_t)in[2] << 8u) | (uint32_t)in[3];
}

uint32_t h2_iperf_read_u32_le(const uint8_t *in) {
    return ((uint32_t)in[3] << 24u) | ((uint32_t)in[2] << 16u) |
           ((uint32_t)in[1] << 8u) | (uint32_t)in[0];
}

void h2_iperf_write_u64_be(uint8_t *out, uint64_t value) {
    h2_iperf_write_u32_be(out, (uint32_t)(value >> 32u));
    h2_iperf_write_u32_be(out + 4, (uint32_t)value);
}

uint64_t h2_iperf_read_u64_be(const uint8_t *in) {
    return ((uint64_t)h2_iperf_read_u32_be(in) << 32u) |
           (uint64_t)h2_iperf_read_u32_be(in + 4);
}

/* ---- control channel ----------------------------------------------------- */

static uint32_t remaining_ms(const h2_iperf_config_t *config, uint64_t deadline_ms) {
    uint64_t now = h2_iperf_now_ms(config);
    if (h2_pal_time_deadline_expired(now, deadline_ms)) {
        return 0u;
    }
    uint64_t left = deadline_ms - now;
    return left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
}

h2_pal_result_t h2_iperf_ctrl_send_all(
    const h2_iperf_config_t *config,
    h2_pal_net_socket_t sock,
    const uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    uint64_t deadline = h2_iperf_now_ms(config) + timeout_ms;
    size_t offset = 0u;
    while (offset < len) {
        uint32_t left = remaining_ms(config, deadline);
        if (left == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
        int sent = h2_pal_net_tcp_send_timeout(
            config->net, sock, data + offset, len - offset, left);
        if (sent == H2_PAL_ERR_UNSUPPORTED) {
            sent = h2_pal_net_tcp_send(config->net, sock, data + offset, len - offset);
        }
        if (sent == H2_PAL_ERR_WOULD_BLOCK) {
            h2_iperf_sleep_ms(config, 1u);
            continue;
        }
        if (sent < 0) {
            return (h2_pal_result_t)sent;
        }
        if (sent == 0) {
            return H2_PAL_ERR_CLOSED;
        }
        offset += (size_t)sent;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_iperf_ctrl_recv_all(
    const h2_iperf_config_t *config,
    h2_pal_net_socket_t sock,
    uint8_t *data,
    size_t len,
    uint32_t timeout_ms) {
    uint64_t deadline = h2_iperf_now_ms(config) + timeout_ms;
    size_t offset = 0u;
    while (offset < len) {
        uint32_t left = remaining_ms(config, deadline);
        if (left == 0u) {
            return H2_PAL_ERR_TIMEOUT;
        }
        int got = h2_pal_net_tcp_recv(config->net, sock, data + offset, len - offset, left);
        if (got == H2_PAL_ERR_WOULD_BLOCK) {
            continue;
        }
        if (got < 0) {
            return (h2_pal_result_t)got;
        }
        if (got == 0) {
            return H2_PAL_ERR_CLOSED;
        }
        offset += (size_t)got;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_iperf_ctrl_send_state(
    const h2_iperf_config_t *config,
    h2_pal_net_socket_t sock,
    int8_t state,
    uint32_t timeout_ms) {
    uint8_t byte = (uint8_t)state;
    return h2_iperf_ctrl_send_all(config, sock, &byte, 1u, timeout_ms);
}

h2_pal_result_t h2_iperf_ctrl_recv_state(
    const h2_iperf_config_t *config,
    h2_pal_net_socket_t sock,
    int8_t *out_state,
    uint32_t timeout_ms) {
    uint8_t byte = 0u;
    int got = h2_pal_net_tcp_recv(config->net, sock, &byte, 1u, timeout_ms);
    if (got == 1) {
        *out_state = (int8_t)byte;
        return H2_PAL_OK;
    }
    if (got == 0) {
        return H2_PAL_ERR_CLOSED;
    }
    if (got == H2_PAL_ERR_WOULD_BLOCK) {
        return H2_PAL_ERR_TIMEOUT;
    }
    return (h2_pal_result_t)got;
}

h2_pal_result_t h2_iperf_ctrl_send_json(
    const h2_iperf_config_t *config,
    h2_pal_net_socket_t sock,
    const char *json,
    size_t len,
    uint32_t timeout_ms) {
    uint8_t header[4];
    if (len > UINT32_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_iperf_write_u32_be(header, (uint32_t)len);
    h2_pal_result_t result =
        h2_iperf_ctrl_send_all(config, sock, header, sizeof(header), timeout_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    return h2_iperf_ctrl_send_all(config, sock, (const uint8_t *)json, len, timeout_ms);
}

h2_pal_result_t h2_iperf_ctrl_recv_json(
    const h2_iperf_config_t *config,
    h2_pal_net_socket_t sock,
    char **out_json,
    size_t *out_len,
    uint32_t timeout_ms) {
    uint8_t header[4];
    *out_json = NULL;
    *out_len = 0u;
    h2_pal_result_t result =
        h2_iperf_ctrl_recv_all(config, sock, header, sizeof(header), timeout_ms);
    if (result != H2_PAL_OK) {
        return result;
    }
    uint32_t len = h2_iperf_read_u32_be(header);
    if (len == 0u || len > H2_IPERF_MAX_JSON_LEN) {
        return H2_PAL_ERR_FORMAT;
    }
    char *json = h2_pal_mem_alloc(config->mem, (size_t)len + 1u);
    if (json == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    result = h2_iperf_ctrl_recv_all(config, sock, (uint8_t *)json, len, timeout_ms);
    if (result != H2_PAL_OK) {
        h2_pal_mem_free(config->mem, json);
        return result;
    }
    json[len] = '\0';
    *out_json = json;
    *out_len = len;
    return H2_PAL_OK;
}

h2_pal_result_t h2_iperf_ctrl_send_server_error(
    const h2_iperf_config_t *config,
    h2_pal_net_socket_t sock,
    int32_t iperf_errno,
    uint32_t timeout_ms) {
    uint8_t payload[9];
    payload[0] = (uint8_t)(int8_t)H2_IPERF_STATE_SERVER_ERROR;
    h2_iperf_write_u32_be(payload + 1, (uint32_t)iperf_errno);
    h2_iperf_write_u32_be(payload + 5, 0u);
    return h2_iperf_ctrl_send_all(config, sock, payload, sizeof(payload), timeout_ms);
}

/* ---- result JSON --------------------------------------------------------- */

bool h2_iperf_build_results_json(
    char *buf,
    size_t cap,
    bool sender,
    const h2_iperf_stream_stats_t *stats) {
    h2_iperf_json_writer_t w;
    h2_iperf_json_init(&w, buf, cap);
    h2_iperf_json_object_begin(&w);
    h2_iperf_json_key(&w, "cpu_util_total");
    h2_iperf_json_f64(&w, 0.0);
    h2_iperf_json_key(&w, "cpu_util_user");
    h2_iperf_json_f64(&w, 0.0);
    h2_iperf_json_key(&w, "cpu_util_system");
    h2_iperf_json_f64(&w, 0.0);
    h2_iperf_json_key(&w, "sender_has_retransmits");
    h2_iperf_json_i64(&w, sender ? 0 : -1);
    h2_iperf_json_key(&w, "streams");
    h2_iperf_json_array_begin(&w);
    h2_iperf_json_object_begin(&w);
    h2_iperf_json_key(&w, "id");
    h2_iperf_json_i64(&w, 1);
    h2_iperf_json_key(&w, "bytes");
    h2_iperf_json_u64(&w, stats->bytes);
    h2_iperf_json_key(&w, "retransmits");
    h2_iperf_json_i64(&w, -1);
    h2_iperf_json_key(&w, "jitter");
    h2_iperf_json_f64(&w, stats->jitter_ms / 1000.0);
    h2_iperf_json_key(&w, "errors");
    h2_iperf_json_i64(&w, stats->lost_packets < 0 ? 0 : stats->lost_packets);
    h2_iperf_json_key(&w, "omitted_errors");
    h2_iperf_json_i64(&w, 0);
    h2_iperf_json_key(&w, "packets");
    h2_iperf_json_u64(&w, stats->packets);
    h2_iperf_json_key(&w, "omitted_packets");
    h2_iperf_json_i64(&w, 0);
    h2_iperf_json_key(&w, "start_time");
    h2_iperf_json_f64(&w, 0.0);
    h2_iperf_json_key(&w, "end_time");
    h2_iperf_json_f64(&w, (double)stats->duration_ms / 1000.0);
    h2_iperf_json_object_end(&w);
    h2_iperf_json_array_end(&w);
    h2_iperf_json_object_end(&w);
    return h2_iperf_json_finish(&w);
}

bool h2_iperf_parse_results_json(
    const char *json,
    size_t len,
    h2_iperf_stream_stats_t *stats) {
    const char *streams = NULL;
    size_t streams_len = 0u;
    memset(stats, 0, sizeof(*stats));
    stats->lost_packets = -1;
    stats->retransmits = -1;
    if (!h2_iperf_json_find(json, len, "streams", &streams, &streams_len)) {
        return false;
    }
    int64_t value = 0;
    double number = 0.0;
    if (!h2_iperf_json_get_i64(streams, streams_len, "bytes", &value)) {
        return false;
    }
    stats->bytes = value < 0 ? 0u : (uint64_t)value;
    if (h2_iperf_json_get_i64(streams, streams_len, "packets", &value)) {
        stats->packets = value < 0 ? 0u : (uint64_t)value;
    }
    if (h2_iperf_json_get_i64(streams, streams_len, "errors", &value)) {
        stats->lost_packets = value;
    }
    if (h2_iperf_json_get_i64(streams, streams_len, "retransmits", &value)) {
        stats->retransmits = value > INT32_MAX ? INT32_MAX : (int32_t)value;
    }
    if (h2_iperf_json_get_f64(streams, streams_len, "jitter", &number)) {
        stats->jitter_ms = number * 1000.0;
    }
    double start_time = 0.0;
    double end_time = 0.0;
    if (h2_iperf_json_get_f64(streams, streams_len, "start_time", &start_time) &&
        h2_iperf_json_get_f64(streams, streams_len, "end_time", &end_time) &&
        end_time >= start_time) {
        stats->duration_ms = (uint32_t)((end_time - start_time) * 1000.0 + 0.5);
    }
    return true;
}
