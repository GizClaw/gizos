#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "h2_h2loader_host.h"
#include "h2_iostreamikcp.h"

#define H2_H2LOADER_HOST_SERIAL_POLL_MS 10u

struct h2_h2loader_host_serial_connection {
    const h2_pal_time_api_t *time;
    h2_iostreamikcp_t *stream;
    h2_h2loader_host_transport_log_fn on_log;
    void *log_user;
    size_t ready_prefix_len;
    int ready_line_rejected;
    int ready_banner_pending;
};

struct h2_iostreamikcp {
    const uint8_t *seed;
    size_t len;
    size_t offset;
    const uint8_t *raw;
    size_t raw_len;
    size_t raw_offset;
    void *log_user;
};

static h2_pal_result_t serial_stream_log(
    void *user, const uint8_t *data, size_t len);

h2_pal_result_t h2_iostreamikcp_poll(
    h2_iostreamikcp_t *stream, uint32_t timeout_ms) {
    (void)timeout_ms;
    size_t remaining = stream->raw_len - stream->raw_offset;
    if (remaining > 0u) {
        size_t chunk_len = remaining < 8u ? remaining : 8u;
        const uint8_t *chunk = stream->raw + stream->raw_offset;
        stream->raw_offset += chunk_len;
        return serial_stream_log(stream->log_user, chunk, chunk_len);
    }
    return H2_PAL_ERR_TIMEOUT;
}

h2_pal_result_t h2_iostreamikcp_update(
    h2_iostreamikcp_t *stream, uint32_t now_ms) {
    (void)stream;
    (void)now_ms;
    return H2_PAL_OK;
}

h2_pal_result_t h2_iostreamikcp_read(
    h2_iostreamikcp_t *stream, uint8_t *out, size_t out_size, size_t *out_len) {
    size_t remaining = stream->len - stream->offset;
    *out_len = remaining < out_size ? remaining : out_size;
    if (remaining == 0u) {
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    memcpy(out, stream->seed + stream->offset, *out_len);
    stream->offset += *out_len;
    return H2_PAL_OK;
}

/* SERIAL_NOW */

/* SERIAL_PUMP */

/* SERIAL_LOG */

/* MONITOR */

typedef struct log_capture {
    uint8_t bytes[4096];
    size_t len;
} log_capture_t;

static h2_pal_result_t capture_log(void *user, const uint8_t *data, size_t len) {
    log_capture_t *logs = user;
    assert(logs->len + len <= sizeof(logs->bytes));
    memcpy(logs->bytes + logs->len, data, len);
    logs->len += len;
    return H2_PAL_OK;
}

static h2_pal_result_t monotonic_ms(void *user, uint64_t *out_ms) {
    uint64_t *now = user;
    *now += 10u;
    *out_ms = *now;
    return H2_PAL_OK;
}

static int cancel_after_polls(void *user) {
    size_t *polls = user;
    if (*polls == 0u) {
        return 1;
    }
    --*polls;
    return 0;
}

static void check_monitor(
    const char *raw, size_t raw_len, const char *seed, size_t len,
    h2_pal_result_t expected, const char *combined, size_t combined_len) {
    uint64_t now = 0u;
    const h2_pal_time_vtable_t vtable = { .get_monotonic_ms = monotonic_ms };
    const h2_pal_time_api_t time = { .user = &now, .vtable = &vtable };
    h2_iostreamikcp_t stream = {
        .seed = (const uint8_t *)seed,
        .len = len,
        .raw = (const uint8_t *)raw,
        .raw_len = raw_len,
    };
    log_capture_t logs = { 0 };
    size_t polls = raw_len / 8u + 2u;
    h2_h2loader_host_serial_connection_t connection = {
        .time = &time,
        .stream = &stream,
        .on_log = capture_log,
        .log_user = &logs,
    };
    stream.log_user = &connection;
    assert(h2_h2loader_host_serial_monitor_logs(
        &connection, cancel_after_polls, &polls) == expected);
    assert(now > 0u);
    assert(logs.len == combined_len);
    assert(memcmp(logs.bytes, combined, combined_len) == 0);
    assert(stream.offset == len);
    assert(stream.raw_offset == raw_len);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    /* Keep drain-removal mutation checks focused on runtime assertions. */
    (void)serial_drain_decoded_logs;
    if (strcmp(argv[1], "tunnelled") == 0) {
        static const char text[] =
            "H2_PAL_E2E suite=1 case=1 result=0\r\n"
            "H2_PAL_E2E suite=1 case=2 result=0\r\n";
        check_monitor("", 0u, text, sizeof(text) - 1u,
            H2_PAL_EXIT, text, sizeof(text) - 1u);
        char large[1600];
        size_t len = 0u;
        for (unsigned index = 0u; index < 43u; ++index) {
            int n = snprintf(large + len, sizeof(large) - len,
                "H2_PAL_E2E suite=1 case=%02u result=0\r\n", index);
            assert(n > 0 && (size_t)n < sizeof(large) - len);
            len += (size_t)n;
        }
        assert(len > 1500u);
        check_monitor("", 0u, large, len, H2_PAL_EXIT, large, len);
    } else if (strcmp(argv[1], "mixed") == 0) {
        static const char raw[] = "console\nordinary console output\r\n";
        static const char decoded[] = "decoded console output\r\n";
        static const char interleaved[] =
            "console\ndecoded console output\r\nordinary console output\r\n";
        /* The first raw chunk precedes the entire decoded drain; subsequent
         * iterations forward the remaining raw chunks. */
        check_monitor(raw, sizeof(raw) - 1u, decoded, sizeof(decoded) - 1u,
            H2_PAL_EXIT, interleaved, sizeof(interleaved) - 1u);
        static const char raw_line[] = "console\n";
        static const char text[] =
            "H2_PAL_E2E suite=1 case=1 result=0\r\n"
            "H2_LOADER_READY board=fake target=fake\r\n";
        static const char combined[] =
            "console\n"
            "H2_PAL_E2E suite=1 case=1 result=0\r\n"
            "H2_LOADER_READY board=fake target=fake\r\n";
        check_monitor(raw_line, sizeof(raw_line) - 1u, text, sizeof(text) - 1u,
            H2_PAL_ERR_CLOSED, combined, sizeof(combined) - 1u);
    } else {
        assert(strcmp(argv[1], "raw_only") == 0);
        static const char raw[] = "ordinary console output across several polls\r\n";
        check_monitor(raw, sizeof(raw) - 1u, "", 0u,
            H2_PAL_EXIT, raw, sizeof(raw) - 1u);
    }
    return 0;
}
