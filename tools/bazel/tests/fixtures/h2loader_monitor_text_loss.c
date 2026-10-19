#include <assert.h>
#include <string.h>
#include "h2_h2loader_host.h"
#include "h2_iostreamikcp.h"

/* FRAME */

struct h2_h2loader_host_serial_connection {
    h2_h2loader_host_transport_log_fn on_log;
    void *log_user;
    size_t ready_prefix_len;
    int ready_line_rejected;
    int ready_banner_pending;
};

/* SERIAL_LOG */

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

int main(int argc, char **argv) {
    assert(argc == 2);
    size_t chunk = strcmp(argv[1], "bytewise") == 0 ? 1u : 256u;
    static const char text[] =
        "H2_JIELI_BUTTON_SMOKE_READY buttons=8 display=480x320 result=0\r\n"
        "JIELI_APP_CONFIRM result=OK code=0 target=0 transport=0\r\n";
    static const char ready[] = "H2_LOADER_READY board=fake target=fake\r\n";
    uint8_t input[1024];
    /* Put the broken candidate across a read boundary. */
    memset(input, 'x', 198u);
    input[198] = '\r';
    input[199] = '\n';
    size_t len = 200u;
    h2_iostreamikcp_frame_t frame = {
        .flags = H2_IOSTREAMIKCP_FRAME_FLAG_DATA,
        .conv = 7u,
        .payload = (const uint8_t *)text,
        .payload_len = sizeof(text) - 1u,
    };
    size_t encoded_len = 0u;
    assert(h2_iostreamikcp_frame_encode(
        &frame, input + len, sizeof(input) - len, &encoded_len) == H2_PAL_OK);
    input[len + 14u] ^= 0xffu;
    len += encoded_len;
    size_t ready_offset = len;
    memcpy(input + len, ready, sizeof(ready) - 1u);
    len += sizeof(ready) - 1u;
    log_capture_t logs = { 0 };
    h2_h2loader_host_serial_connection_t connection = {
        .on_log = capture_log,
        .log_user = &logs,
    };
    h2_iostreamikcp_filter_t filter;
    h2_iostreamikcp_filter_init(&filter);
    h2_pal_result_t rc = H2_PAL_OK;
    for (size_t offset = 0u; offset < len; offset += chunk) {
        size_t n = len - offset < chunk ? len - offset : chunk;
        rc = h2_iostreamikcp_filter_input_with_log(
            &filter, input + offset, n, NULL, NULL, serial_stream_log, &connection);
        assert(rc == (offset + n == len ? H2_PAL_ERR_CLOSED : H2_PAL_OK));
    }
    assert(rc == H2_PAL_ERR_CLOSED);
    assert(filter.frames == 0u);
    assert(filter.crc_errors == 1u);
    assert(logs.len == len);
    assert(memcmp(logs.bytes, input, len) == 0);
    assert(memcmp(logs.bytes + 200u + H2_IOSTREAMIKCP_FRAME_HEADER_LEN,
                  text, sizeof(text) - 1u) == 0);
    assert(memcmp(logs.bytes + ready_offset, ready, sizeof(ready) - 1u) == 0);
    return 0;
}
