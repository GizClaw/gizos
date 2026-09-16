#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "h2_iostreamikcp.h"
#define H2_WRITE_TIMEOUT_MS 5000u
#define H2_MAX_FLUSH_TIMEOUT_MS 120000u
#define H2_SEGMENT_TIMEOUT_MS 60u
#define H2_MAX_WAITSND 64u
#define H2_PHYSICAL_POLL_MS 10u
#define H2_STREAM_RX_SIZE 4096u
#define H2_STREAM_RECEIVE_WINDOW 20u
static uint32_t clock_ms, observed, writes, first_timeout;
static int stop_requested, window_full, two_frames, read_case, control_case, expired_case;
struct h2_iostreamikcp { h2_iostreamikcp_config_t config; };
/* STRUCTURE */
static uint32_t timer_get_ms(void) { return clock_ms; }
static uint32_t fixture_now(void *user) { (void)user; return clock_ms; }
static h2_pal_result_t fake_read(void *user, void *buffer, size_t len, size_t *count, uint32_t timeout) {
    (void)user;
    (void)buffer;
    (void)len;
    (void)timeout;
    *count = 0;
    return H2_PAL_ERR_TIMEOUT;
}
static h2_pal_result_t fake_write(void *user, const void *buffer, size_t len, size_t *count, uint32_t timeout) {
    (void)user;
    (void)buffer;
    observed = timeout;
    ++writes;
    if (writes == 1u) first_timeout = timeout;
    if (two_frames && writes == 1u) {
        clock_ms += 3u;
        *count = len;
        return H2_PAL_OK;
    }
    clock_ms += timeout;
    *count = 0;
    return H2_PAL_ERR_TIMEOUT;
}
h2_pal_result_t h2_iostreamikcp_get_stats(h2_iostreamikcp_t *stream, h2_iostreamikcp_stats_t *out) {
    (void)stream;
    memset(out, 0, sizeof(*out));
    out->waitsnd = window_full ? H2_MAX_WAITSND : 0u;
    return H2_PAL_OK;
}
h2_pal_result_t h2_iostreamikcp_write(h2_iostreamikcp_t *stream, const uint8_t *data, size_t len) {
    size_t count = 0;
    int result = stream->config.io.write(stream->config.io.user, data, len, &count, stream->config.write_timeout_ms);
    if (two_frames && result == H2_PAL_OK) {
        result = stream->config.io.write(stream->config.io.user, data, len, &count, stream->config.write_timeout_ms);
    }
    return result;
}
h2_pal_result_t h2_iostreamikcp_flush(h2_iostreamikcp_t *stream) {
    return h2_iostreamikcp_write(stream, (const uint8_t *)"a", 1u);
}
h2_pal_result_t h2_iostreamikcp_update(h2_iostreamikcp_t *stream, uint32_t now) {
    (void)now;
    return h2_iostreamikcp_flush(stream);
}
static void write_le32(uint8_t *data, uint32_t value) { memcpy(data, &value, sizeof(value)); }
h2_pal_result_t h2_iostreamikcp_frame_encode(const h2_iostreamikcp_frame_t *frame, uint8_t *out, size_t size, size_t *len) {
    (void)frame;
    assert(size > 0u);
    out[0] = 1;
    *len = 1;
    return H2_PAL_OK;
}
h2_pal_result_t h2_iostreamikcp_read(h2_iostreamikcp_t *stream, uint8_t *out, size_t size, size_t *len) {
    (void)stream;
    (void)out;
    (void)size;
    *len = 0;
    return H2_PAL_ERR_WOULD_BLOCK;
}
static int send_control(TRANSPORT *self, uint8_t flags, uint32_t conv);
static int poll_physical(TRANSPORT *self, uint32_t timeout) {
    (void)self;
    clock_ms += expired_case ? timeout : (timeout < 3u ? timeout : 3u);
    window_full = 0;
    if (control_case) return send_control(self, H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK, 1u);
    if (read_case) return h2_iostreamikcp_write(self->stream, (const uint8_t *)"a", 1u);
    return H2_PAL_ERR_TIMEOUT;
}
/* FUNCTIONS */
static void configure(TRANSPORT *self, h2_iostreamikcp_t *stream) {
    uint32_t conv = 1u;
    /* CONFIG */
    stream->config = config;
    self->stream = stream;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    (void)stop_requested;
    TRANSPORT transport = {.physical_io = {.read = fake_read, .write = fake_write}};
    h2_iostreamikcp_t stream;
    configure(&transport, &stream);
    uint32_t budget = 5u;
    if (strcmp(argv[1], "zero") == 0 || strcmp(argv[1], "zero_window") == 0) budget = 0u;
    window_full = strcmp(argv[1], "window") == 0 || strcmp(argv[1], "zero_window") == 0;
    read_case = strcmp(argv[1], "read") == 0;
    control_case = strcmp(argv[1], "control") == 0;
    expired_case = strcmp(argv[1], "expired") == 0;
    if (control_case || expired_case) window_full = 1;
    two_frames = strcmp(argv[1], "two_frames") == 0;
    size_t written = 99u;
    char byte;
    transport.write_timeout_ms = budget;
    int result = strcmp(argv[1], "flush") == 0 ? command_flush(&transport) : read_case
        ? command_read(&transport, &byte, 1u, &written, budget)
        : command_write(&transport, "a", 1u, &written, budget);
    assert(result == H2_PAL_ERR_TIMEOUT);
    assert(clock_ms <= budget);
    if (expired_case) {
        assert(writes == 0u);
    } else if (control_case || read_case) {
        assert(writes == 1u);
        assert(first_timeout == 2u);
    } else if (strcmp(argv[1], "zero_window") == 0) {
        assert(writes == 0u);
    } else if (two_frames || strcmp(argv[1], "window") == 0) {
        assert(observed == 2u);
    } else {
        assert(observed == budget);
    }
    return 0;
}
