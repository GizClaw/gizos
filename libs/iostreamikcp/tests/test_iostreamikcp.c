#include "h2_iostreamikcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

typedef struct memory_io {
    uint8_t bytes[8192];
    size_t len;
    size_t read_pos;
    h2_pal_result_t write_result;
    size_t write_count;
    h2_pal_result_t flush_result;
    size_t flush_count;
} memory_io_t;

typedef struct frame_capture {
    uint8_t flags[8];
    uint32_t conv[8];
    uint8_t payload[8][128];
    size_t len[8];
    size_t count;
} frame_capture_t;

typedef struct log_capture {
    uint8_t bytes[64];
    size_t len;
    h2_pal_result_t result;
} log_capture_t;

static h2_pal_result_t capture_log(void *user, const uint8_t *data, size_t len) {
    log_capture_t *capture = (log_capture_t *)user;
    if (capture->result != H2_PAL_OK) return capture->result;
    CHECK(capture->len + len <= sizeof(capture->bytes));
    memcpy(&capture->bytes[capture->len], data, len);
    capture->len += len;
    return H2_PAL_OK;
}

static h2_pal_result_t memory_read(
    void *user,
    void *buffer,
    size_t len,
    size_t *out_read,
    uint32_t timeout_ms) {
    (void)timeout_ms;
    memory_io_t *io = (memory_io_t *)user;
    size_t avail = io->len - io->read_pos;
    size_t n = avail < len ? avail : len;
    if (n > 0u) {
        memcpy(buffer, io->bytes + io->read_pos, n);
        io->read_pos += n;
    }
    *out_read = n;
    return n > 0u ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t memory_write(
    void *user,
    const void *buffer,
    size_t len,
    size_t *out_written,
    uint32_t timeout_ms) {
    (void)timeout_ms;
    memory_io_t *io = (memory_io_t *)user;
    io->write_count++;
    if (io->write_result != H2_PAL_OK) {
        *out_written = 0u;
        return io->write_result;
    }
    CHECK(io->len + len <= sizeof(io->bytes));
    memcpy(io->bytes + io->len, buffer, len);
    io->len += len;
    *out_written = len;
    return H2_PAL_OK;
}

static h2_pal_result_t memory_flush(void *user) {
    memory_io_t *io = (memory_io_t *)user;
    io->flush_count++;
    return io->flush_result;
}

static h2_pal_result_t capture_frame(void *user, const h2_iostreamikcp_frame_t *frame) {
    frame_capture_t *capture = (frame_capture_t *)user;
    CHECK(capture->count < 8u);
    CHECK(frame->payload_len <= sizeof(capture->payload[0]));
    capture->flags[capture->count] = frame->flags;
    capture->conv[capture->count] = frame->conv;
    capture->len[capture->count] = frame->payload_len;
    memcpy(capture->payload[capture->count], frame->payload, frame->payload_len);
    capture->count++;
    return H2_PAL_OK;
}

static void test_frame_filter_extracts_from_dirty_stream(void) {
    const uint8_t payload[] = { 1u, 2u, 3u, 4u };
    uint8_t encoded[64];
    size_t encoded_len = 0u;
    h2_iostreamikcp_frame_t frame = {
        .conv = 7u,
        .payload = payload,
        .payload_len = sizeof(payload),
    };
    CHECK(h2_iostreamikcp_frame_encode(&frame, encoded, sizeof(encoded), &encoded_len) == H2_PAL_OK);

    h2_iostreamikcp_filter_t filter;
    frame_capture_t capture = { 0 };
    h2_iostreamikcp_filter_init(&filter);
    CHECK(h2_iostreamikcp_filter_input(&filter, (const uint8_t *)"log:", 4u, capture_frame, &capture) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_filter_input(&filter, encoded, 3u, capture_frame, &capture) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_filter_input(&filter, encoded + 3u, encoded_len - 3u, capture_frame, &capture) == H2_PAL_OK);
    CHECK(capture.count == 1u);
    CHECK(capture.flags[0] == H2_IOSTREAMIKCP_FRAME_FLAG_DATA);
    CHECK(capture.conv[0] == 7u);
    CHECK(capture.len[0] == sizeof(payload));
    CHECK(memcmp(capture.payload[0], payload, sizeof(payload)) == 0);
    CHECK(filter.log_bytes == 4u);
}

static void test_filter_delivers_only_proven_log_bytes(void) {
    const uint8_t payload[] = {1u, 2u};
    uint8_t encoded[64];
    size_t encoded_len = 0u;
    h2_iostreamikcp_frame_t frame = {
        .conv = 7u, .payload = payload, .payload_len = sizeof(payload),
    };
    h2_iostreamikcp_filter_t filter;
    frame_capture_t frames = {0};
    log_capture_t logs = {0};
    CHECK(h2_iostreamikcp_frame_encode(&frame, encoded, sizeof(encoded), &encoded_len) == H2_PAL_OK);
    h2_iostreamikcp_filter_init(&filter);
    CHECK(h2_iostreamikcp_filter_input_with_log(
              &filter, (const uint8_t *)"boot\nH2I", 8u,
              capture_frame, &frames, capture_log, &logs) == H2_PAL_OK);
    CHECK(logs.len == 5u);
    CHECK(memcmp(logs.bytes, "boot\n", 5u) == 0);
    CHECK(h2_iostreamikcp_filter_input_with_log(
              &filter, encoded + 3u, encoded_len - 3u,
              capture_frame, &frames, capture_log, &logs) == H2_PAL_OK);
    CHECK(frames.count == 1u);
    CHECK(logs.len == 5u);

    logs.result = H2_PAL_ERR_CLOSED;
    CHECK(h2_iostreamikcp_filter_input_with_log(
              &filter, (const uint8_t *)"x", 1u,
              capture_frame, &frames, capture_log, &logs) == H2_PAL_ERR_CLOSED);
}

static void write_le32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint16_t read_le16(const uint8_t *in) {
    return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

static void test_session_control_frames(void) {
    CHECK(H2_IOSTREAMIKCP_FRAME_FLAG_DATA == 0x00u);
    CHECK(H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN == 0x01u);
    CHECK(H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK == 0x02u);
    CHECK(H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_CLOSE == 0x04u);
    uint8_t payload[H2_IOSTREAMIKCP_SESSION_CONTROL_PAYLOAD_LEN];
    uint8_t encoded[64];
    size_t encoded_len = 0u;
    write_le32(payload, UINT32_C(0x12345678));
    h2_iostreamikcp_frame_t frame = {
        .flags = H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN,
        .conv = UINT32_C(0x12345678),
        .payload = payload,
        .payload_len = sizeof(payload),
    };
    CHECK(h2_iostreamikcp_frame_encode(&frame, encoded, sizeof(encoded), &encoded_len) == H2_PAL_OK);
    CHECK(encoded[7] == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN);

    h2_iostreamikcp_filter_t filter;
    frame_capture_t capture = { 0 };
    h2_iostreamikcp_filter_init(&filter);
    CHECK(h2_iostreamikcp_filter_input(
              &filter, encoded, 2u, capture_frame, &capture) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_filter_input(
              &filter, encoded + 2u, encoded_len - 2u, capture_frame, &capture) == H2_PAL_OK);
    CHECK(capture.count == 1u);
    CHECK(capture.flags[0] == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN);
    CHECK(capture.conv[0] == UINT32_C(0x12345678));

    frame.flags = H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK;
    CHECK(h2_iostreamikcp_frame_encode(&frame, encoded, sizeof(encoded), &encoded_len) == H2_PAL_OK);
    CHECK(encoded[7] == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK);

    frame.flags = H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_CLOSE;
    CHECK(h2_iostreamikcp_frame_encode(&frame, encoded, sizeof(encoded), &encoded_len) == H2_PAL_OK);
    CHECK(encoded[7] == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_CLOSE);

    payload[0] ^= 1u;
    CHECK(h2_iostreamikcp_frame_encode(&frame, encoded, sizeof(encoded), &encoded_len) ==
          H2_PAL_ERR_INVALID_ARG);
    frame.flags = H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN |
                  H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK;
    CHECK(h2_iostreamikcp_frame_encode(&frame, encoded, sizeof(encoded), &encoded_len) ==
          H2_PAL_ERR_INVALID_ARG);
    memset(payload, 0, sizeof(payload));
    frame.flags = H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN;
    frame.conv = 0u;
    CHECK(h2_iostreamikcp_frame_encode(&frame, encoded, sizeof(encoded), &encoded_len) ==
          H2_PAL_ERR_INVALID_ARG);
}

static void test_filter_rejects_invalid_control_frame(void) {
    const uint8_t payload[] = { 0u, 0u, 0u, 0u };
    uint8_t encoded[64];
    size_t encoded_len = 0u;
    h2_iostreamikcp_frame_t data = {
        .conv = 3u,
        .payload = payload,
        .payload_len = sizeof(payload),
    };
    CHECK(h2_iostreamikcp_frame_encode(&data, encoded, sizeof(encoded), &encoded_len) == H2_PAL_OK);
    encoded[7] = H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN;

    h2_iostreamikcp_filter_t filter;
    frame_capture_t capture = { 0 };
    h2_iostreamikcp_filter_init(&filter);
    CHECK(h2_iostreamikcp_filter_input(
              &filter, encoded, encoded_len, capture_frame, &capture) == H2_PAL_OK);
    CHECK(capture.count == 0u);
    CHECK(filter.errors == 1u);

    data.conv = 0u;
    CHECK(h2_iostreamikcp_frame_encode(&data, encoded, sizeof(encoded), &encoded_len) == H2_PAL_OK);
    encoded[7] = H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN;
    h2_iostreamikcp_filter_init(&filter);
    memset(&capture, 0, sizeof(capture));
    CHECK(h2_iostreamikcp_filter_input(
              &filter, encoded, encoded_len, capture_frame, &capture) == H2_PAL_OK);
    CHECK(capture.count == 0u);
    CHECK(filter.errors == 1u);
}

static void test_filter_discards_oversized_binary_candidate(void) {
    uint8_t oversized[H2_IOSTREAMIKCP_FRAME_HEADER_LEN + 4u] = { 0 };
    memcpy(oversized, "H2IKCP", 6u);
    oversized[6] = H2_IOSTREAMIKCP_FRAME_VERSION;
    oversized[12] = 0x01u;
    oversized[13] = 0x04u;

    h2_iostreamikcp_filter_t filter;
    frame_capture_t capture = { 0 };
    h2_iostreamikcp_filter_init(&filter);
    CHECK(h2_iostreamikcp_filter_input(
              &filter, oversized, sizeof(oversized), capture_frame, &capture) == H2_PAL_OK);
    CHECK(capture.count == 0u);
    CHECK(filter.errors == 1u);
    CHECK(filter.log_bytes == 0u);
    const uint8_t payload[] = { 1u };
    h2_iostreamikcp_frame_t good = {
        .flags = H2_IOSTREAMIKCP_FRAME_FLAG_DATA,
        .conv = 9u,
        .payload = payload,
        .payload_len = sizeof(payload),
    };
    uint8_t encoded[32];
    size_t encoded_len = 0u;
    CHECK(h2_iostreamikcp_frame_encode(
              &good, encoded, sizeof(encoded), &encoded_len) == H2_PAL_OK);
    uint8_t remainder[H2_IOSTREAMIKCP_DEFAULT_MTU + sizeof(encoded)] = { 0 };
    memcpy(remainder + H2_IOSTREAMIKCP_DEFAULT_MTU, encoded, encoded_len);
    CHECK(h2_iostreamikcp_filter_input(
              &filter,
              remainder,
              H2_IOSTREAMIKCP_DEFAULT_MTU + encoded_len,
              capture_frame,
              &capture) == H2_PAL_OK);
    CHECK(filter.resyncing == 0);
    CHECK(filter.log_bytes == 0u);
    CHECK(capture.count == 1u);
}

static void test_filter_rejects_false_magic_and_crc(void) {
    const uint8_t payload[] = { 9u, 8u, 7u };
    uint8_t encoded[64];
    size_t encoded_len = 0u;
    h2_iostreamikcp_frame_t frame = {
        .conv = 9u,
        .payload = payload,
        .payload_len = sizeof(payload),
    };
    CHECK(h2_iostreamikcp_frame_encode(&frame, encoded, sizeof(encoded), &encoded_len) == H2_PAL_OK);

    h2_iostreamikcp_filter_t filter;
    frame_capture_t capture = { 0 };
    h2_iostreamikcp_filter_init(&filter);
    const uint8_t false_magic[] = { 'H', '2', 'I', 'X', 'x' };
    CHECK(h2_iostreamikcp_filter_input(&filter, false_magic, sizeof(false_magic), capture_frame, &capture) == H2_PAL_OK);
    CHECK(capture.count == 0u);

    uint64_t log_before = filter.log_bytes;
    encoded[encoded_len - 1u] ^= 0x55u;
    CHECK(h2_iostreamikcp_filter_input(&filter, encoded, encoded_len, capture_frame, &capture) == H2_PAL_OK);
    CHECK(capture.count == 0u);
    CHECK(filter.crc_errors == 1u);
    CHECK(filter.log_bytes == log_before);
}

static void test_filter_resyncs_before_invalid_metadata_length(void) {
    const uint8_t payload[] = { 4u, 2u };
    uint8_t good[64];
    size_t good_len = 0u;
    h2_iostreamikcp_frame_t frame = {
        .conv = 19u,
        .payload = payload,
        .payload_len = sizeof(payload),
    };
    CHECK(h2_iostreamikcp_frame_encode(
              &frame, good, sizeof(good), &good_len) == H2_PAL_OK);

    for (size_t variant = 0u; variant < 2u; ++variant) {
        uint8_t input[H2_IOSTREAMIKCP_FRAME_HEADER_LEN + sizeof(good)] = { 0 };
        memcpy(input, "H2IKCP", 6u);
        input[6] = variant == 0u ?
            H2_IOSTREAMIKCP_FRAME_VERSION + 1u : H2_IOSTREAMIKCP_FRAME_VERSION;
        input[7] = variant == 0u ? H2_IOSTREAMIKCP_FRAME_FLAG_DATA : 0xffu;
        input[12] = (uint8_t)(good_len & 0xffu);
        input[13] = (uint8_t)(good_len >> 8);
        memcpy(input + H2_IOSTREAMIKCP_FRAME_HEADER_LEN, good, good_len);

        h2_iostreamikcp_filter_t filter;
        frame_capture_t capture = { 0 };
        h2_iostreamikcp_filter_init(&filter);
        CHECK(h2_iostreamikcp_filter_input(
                  &filter,
                  input,
                  H2_IOSTREAMIKCP_FRAME_HEADER_LEN + good_len,
                  capture_frame,
                  &capture) == H2_PAL_OK);
        CHECK(filter.errors == 1u);
        CHECK(capture.count == 1u);
        CHECK(capture.conv[0] == 19u);
        CHECK(capture.len[0] == sizeof(payload));
        CHECK(memcmp(capture.payload[0], payload, sizeof(payload)) == 0);
    }
}

static void test_filter_multiple_frames(void) {
    const uint8_t a[] = { 1u };
    const uint8_t b[] = { 2u, 3u };
    uint8_t encoded[128];
    size_t a_len = 0u;
    size_t b_len = 0u;
    h2_iostreamikcp_frame_t frame_a = { .conv = 1u, .payload = a, .payload_len = sizeof(a) };
    h2_iostreamikcp_frame_t frame_b = { .conv = 2u, .payload = b, .payload_len = sizeof(b) };
    CHECK(h2_iostreamikcp_frame_encode(&frame_a, encoded, sizeof(encoded), &a_len) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_frame_encode(&frame_b, encoded + a_len, sizeof(encoded) - a_len, &b_len) == H2_PAL_OK);

    h2_iostreamikcp_filter_t filter;
    frame_capture_t capture = { 0 };
    h2_iostreamikcp_filter_init(&filter);
    CHECK(h2_iostreamikcp_filter_input(&filter, encoded, a_len + b_len, capture_frame, &capture) == H2_PAL_OK);
    CHECK(capture.count == 2u);
    CHECK(capture.conv[0] == 1u);
    CHECK(capture.conv[1] == 2u);
}

static void test_filter_continues_after_corrupt_frame(void) {
    const uint8_t payload[] = { 4u, 5u, 6u };
    uint8_t encoded[128];
    size_t bad_len = 0u;
    size_t good_len = 0u;
    h2_iostreamikcp_frame_t frame = { .conv = 3u, .payload = payload, .payload_len = sizeof(payload) };
    CHECK(h2_iostreamikcp_frame_encode(&frame, encoded, sizeof(encoded), &bad_len) == H2_PAL_OK);
    encoded[bad_len - 1u] ^= 0x11u;
    CHECK(h2_iostreamikcp_frame_encode(&frame, encoded + bad_len, sizeof(encoded) - bad_len, &good_len) == H2_PAL_OK);

    h2_iostreamikcp_filter_t filter;
    frame_capture_t capture = { 0 };
    h2_iostreamikcp_filter_init(&filter);
    CHECK(h2_iostreamikcp_filter_input(&filter, encoded, bad_len + good_len, capture_frame, &capture) == H2_PAL_OK);
    CHECK(filter.crc_errors == 1u);
    CHECK(capture.count == 1u);
    CHECK(capture.conv[0] == 3u);
    CHECK(capture.len[0] == sizeof(payload));
    CHECK(memcmp(capture.payload[0], payload, sizeof(payload)) == 0);
}

static void test_filter_resyncs_to_frame_inside_bad_crc_candidate(void) {
    const uint8_t payload[] = { 9u, 8u, 7u };
    uint8_t good[64];
    size_t good_len = 0u;
    h2_iostreamikcp_frame_t frame = {
        .conv = 19u,
        .payload = payload,
        .payload_len = sizeof(payload),
    };
    CHECK(h2_iostreamikcp_frame_encode(
              &frame, good, sizeof(good), &good_len) == H2_PAL_OK);

    uint8_t input[H2_IOSTREAMIKCP_FRAME_HEADER_LEN + 2u + sizeof(good)] = { 0 };
    memcpy(input, "H2IKCP", 6u);
    input[6] = H2_IOSTREAMIKCP_FRAME_VERSION;
    input[7] = H2_IOSTREAMIKCP_FRAME_FLAG_DATA;
    input[8] = 7u;
    size_t false_payload_len = 2u + good_len;
    input[12] = (uint8_t)(false_payload_len & 0xffu);
    input[13] = (uint8_t)(false_payload_len >> 8);
    memcpy(input + H2_IOSTREAMIKCP_FRAME_HEADER_LEN + 2u, good, good_len);

    h2_iostreamikcp_filter_t filter;
    frame_capture_t capture = { 0 };
    h2_iostreamikcp_filter_init(&filter);
    CHECK(h2_iostreamikcp_filter_input(
              &filter,
              input,
              H2_IOSTREAMIKCP_FRAME_HEADER_LEN + false_payload_len,
              capture_frame,
              &capture) == H2_PAL_OK);
    CHECK(filter.crc_errors == 1u);
    CHECK(filter.log_bytes == 0u);
    CHECK(capture.count == 1u);
    CHECK(capture.conv[0] == 19u);
    CHECK(capture.len[0] == sizeof(payload));
    CHECK(memcmp(capture.payload[0], payload, sizeof(payload)) == 0);
}

static h2_iostreamikcp_t *open_stream_ex(
    memory_io_t *io,
    uint32_t conv,
    size_t mtu,
    size_t rx_size,
    uint32_t receive_window) {
    h2_iostreamikcp_t *stream = NULL;
    h2_iostreamikcp_config_t config = {
        .io = {
            .user = io,
            .read = memory_read,
            .write = memory_write,
            .flush = memory_flush,
        },
        .conv = conv,
        .mtu = mtu,
        .rx_buffer_size = rx_size,
        .receive_window = receive_window,
        .write_timeout_ms = 100u,
    };
    CHECK(h2_iostreamikcp_open(&config, &stream) == H2_PAL_OK);
    return stream;
}

static h2_iostreamikcp_t *open_stream(memory_io_t *io, uint32_t conv) {
    return open_stream_ex(io, conv, 256u, 2048u, 0u);
}

static void test_configured_receive_window_is_advertised(void) {
    memory_io_t a_to_b = { 0 };
    memory_io_t b_to_a = { 0 };
    h2_iostreamikcp_t *a = open_stream(&a_to_b, 2424u);
    h2_iostreamikcp_t *b = open_stream_ex(&b_to_a, 2424u, 256u, 2048u, 24u);
    const uint8_t payload[] = "window";

    CHECK(h2_iostreamikcp_write(a, payload, sizeof(payload)) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_update(a, 20u) == H2_PAL_OK);
    CHECK(a_to_b.len > 0u);
    CHECK(h2_iostreamikcp_input(b, a_to_b.bytes, a_to_b.len) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_update(b, 20u) == H2_PAL_OK);
    CHECK(b_to_a.len >= H2_IOSTREAMIKCP_FRAME_HEADER_LEN + 24u);
    CHECK(read_le16(b_to_a.bytes + H2_IOSTREAMIKCP_FRAME_HEADER_LEN + 6u) == 24u);

    h2_iostreamikcp_close(a);
    h2_iostreamikcp_close(b);
}

static h2_pal_result_t input_decoded_frame(void *user, const h2_iostreamikcp_frame_t *frame) {
    return h2_iostreamikcp_input_frame((h2_iostreamikcp_t *)user, frame);
}

static void test_decoded_data_frame_input(void) {
    memory_io_t a_to_b = { 0 };
    memory_io_t b_to_a = { 0 };
    h2_iostreamikcp_t *a = open_stream(&a_to_b, 4242u);
    h2_iostreamikcp_t *b = open_stream(&b_to_a, 4242u);
    const uint8_t payload[] = "decoded frame input";
    CHECK(h2_iostreamikcp_write(a, payload, sizeof(payload)) == H2_PAL_OK);

    h2_iostreamikcp_filter_t filter;
    h2_iostreamikcp_filter_init(&filter);
    CHECK(h2_iostreamikcp_filter_input(
              &filter, a_to_b.bytes, a_to_b.len, input_decoded_frame, b) == H2_PAL_OK);
    uint8_t out[64];
    size_t out_len = 0u;
    CHECK(h2_iostreamikcp_read(b, out, sizeof(out), &out_len) == H2_PAL_OK);
    CHECK(out_len == sizeof(payload));
    CHECK(memcmp(out, payload, sizeof(payload)) == 0);

    uint8_t control_payload[4];
    write_le32(control_payload, 4242u);
    h2_iostreamikcp_frame_t control = {
        .flags = H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN,
        .conv = 4242u,
        .payload = control_payload,
        .payload_len = sizeof(control_payload),
    };
    CHECK(h2_iostreamikcp_input_frame(b, &control) == H2_PAL_ERR_INVALID_ARG);
    control.flags = H2_IOSTREAMIKCP_FRAME_FLAG_DATA;
    control.conv++;
    CHECK(h2_iostreamikcp_input_frame(b, &control) == H2_PAL_ERR_INVALID_ARG);

    h2_iostreamikcp_close(a);
    h2_iostreamikcp_close(b);
}

static void test_kcp_reconstructs_stream(void) {
    memory_io_t a_to_b = { 0 };
    memory_io_t b_to_a = { 0 };
    h2_iostreamikcp_t *a = open_stream(&a_to_b, 1234u);
    h2_iostreamikcp_t *b = open_stream(&b_to_a, 1234u);

    const uint8_t payload[] = "hello over a dirty stream";
    CHECK(h2_iostreamikcp_write(a, payload, sizeof(payload)) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_update(a, 20u) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_flush(a) == H2_PAL_OK);
    CHECK(a_to_b.len > 0u);
    const uint8_t log_prefix[] = "boot log\n";
    CHECK(h2_iostreamikcp_input(b, log_prefix, sizeof(log_prefix) - 1u) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_input(b, a_to_b.bytes, a_to_b.len) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_update(b, 20u) == H2_PAL_OK);

    uint8_t out[64];
    size_t out_len = 0u;
    CHECK(h2_iostreamikcp_read(b, out, sizeof(out), &out_len) == H2_PAL_OK);
    CHECK(out_len == sizeof(payload));
    CHECK(memcmp(out, payload, sizeof(payload)) == 0);

    h2_iostreamikcp_stats_t stats;
    CHECK(h2_iostreamikcp_get_stats(b, &stats) == H2_PAL_OK);
    CHECK(stats.input_log_bytes == sizeof(log_prefix) - 1u);
    CHECK(stats.rx_frames > 0u);

    h2_iostreamikcp_close(a);
    h2_iostreamikcp_close(b);
}

static void test_kcp_drops_malformed_frame_and_recovers(void) {
    memory_io_t a_to_b = { 0 };
    memory_io_t b_to_a = { 0 };
    h2_iostreamikcp_t *a = open_stream(&a_to_b, 5150u);
    h2_iostreamikcp_t *b = open_stream(&b_to_a, 5150u);

    const uint8_t bad_payload[] = { 1u, 2u, 3u, 4u };
    uint8_t bad_frame[64];
    size_t bad_frame_len = 0u;
    h2_iostreamikcp_frame_t frame = {
        .conv = 5150u,
        .payload = bad_payload,
        .payload_len = sizeof(bad_payload),
    };
    CHECK(h2_iostreamikcp_frame_encode(&frame, bad_frame, sizeof(bad_frame), &bad_frame_len) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_input(b, bad_frame, bad_frame_len) == H2_PAL_ERR_FORMAT);

    h2_iostreamikcp_stats_t stats;
    CHECK(h2_iostreamikcp_get_stats(b, &stats) == H2_PAL_OK);
    CHECK(stats.input_errors == 1u);

    const uint8_t payload[] = "recover after malformed kcp";
    CHECK(h2_iostreamikcp_write(a, payload, sizeof(payload)) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_update(a, 20u) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_flush(a) == H2_PAL_OK);
    CHECK(a_to_b.len > 0u);
    CHECK(h2_iostreamikcp_input(b, a_to_b.bytes, a_to_b.len) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_update(b, 20u) == H2_PAL_OK);

    uint8_t out[64];
    size_t out_len = 0u;
    CHECK(h2_iostreamikcp_read(b, out, sizeof(out), &out_len) == H2_PAL_OK);
    CHECK(out_len == sizeof(payload));
    CHECK(memcmp(out, payload, sizeof(payload)) == 0);

    h2_iostreamikcp_close(a);
    h2_iostreamikcp_close(b);
}

static void test_stats_include_filter_errors(void) {
    memory_io_t io = { 0 };
    h2_iostreamikcp_t *stream = open_stream(&io, 6060u);

    const uint8_t payload[] = { 9u, 8u, 7u };
    uint8_t encoded[64];
    size_t encoded_len = 0u;
    h2_iostreamikcp_frame_t frame = {
        .conv = 6060u,
        .payload = payload,
        .payload_len = sizeof(payload),
    };
    CHECK(h2_iostreamikcp_frame_encode(&frame, encoded, sizeof(encoded), &encoded_len) == H2_PAL_OK);
    encoded[6] ^= 0x7fu;
    CHECK(h2_iostreamikcp_input(stream, encoded, encoded_len) == H2_PAL_OK);

    h2_iostreamikcp_stats_t stats;
    CHECK(h2_iostreamikcp_get_stats(stream, &stats) == H2_PAL_OK);
    CHECK(stats.input_errors == 1u);
    CHECK(stats.crc_errors == 0u);

    h2_iostreamikcp_close(stream);
}

static void test_kcp_chunks_large_writes_and_drains_after_read(void) {
    memory_io_t a_to_b = { 0 };
    memory_io_t b_to_a = { 0 };
    h2_iostreamikcp_t *a = open_stream_ex(&a_to_b, 4321u, 128u, 256u, 0u);
    h2_iostreamikcp_t *b = open_stream_ex(&b_to_a, 4321u, 128u, 256u, 0u);

    uint8_t payload[700];
    for (size_t i = 0u; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)(i & 0xffu);
    }
    CHECK(h2_iostreamikcp_write(a, payload, sizeof(payload)) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_update(a, 20u) == H2_PAL_OK);

    uint8_t out[sizeof(payload)];
    size_t total = 0u;
    size_t a_sent = 0u;
    size_t b_sent = 0u;
    for (uint32_t step = 0u; step < 100u && total < sizeof(out); ++step) {
        uint32_t now = 20u + step * 10u;
        if (a_sent < a_to_b.len) {
            CHECK(h2_iostreamikcp_input(
                      b, a_to_b.bytes + a_sent, a_to_b.len - a_sent) == H2_PAL_OK);
            a_sent = a_to_b.len;
        }
        CHECK(h2_iostreamikcp_update(b, now) == H2_PAL_OK);
        if (b_sent < b_to_a.len) {
            CHECK(h2_iostreamikcp_input(
                      a, b_to_a.bytes + b_sent, b_to_a.len - b_sent) == H2_PAL_OK);
            b_sent = b_to_a.len;
        }
        CHECK(h2_iostreamikcp_update(a, now) == H2_PAL_OK);

        for (;;) {
            size_t out_len = 0u;
            size_t want = sizeof(out) - total < 100u ? sizeof(out) - total : 100u;
            h2_pal_result_t rc = h2_iostreamikcp_read(b, out + total, want, &out_len);
            if (rc == H2_PAL_ERR_WOULD_BLOCK) {
                break;
            }
            CHECK(rc == H2_PAL_OK);
            CHECK(out_len > 0u);
            total += out_len;
            if (total == sizeof(out)) {
                break;
            }
        }
    }
    CHECK(total == sizeof(payload));
    CHECK(memcmp(out, payload, sizeof(payload)) == 0);

    h2_iostreamikcp_close(a);
    h2_iostreamikcp_close(b);
}

static void test_write_reports_transport_failure(void) {
    memory_io_t io = { .write_result = H2_PAL_ERR_TIMEOUT };
    h2_iostreamikcp_t *stream = open_stream(&io, 88u);
    const uint8_t payload[] = { 1u, 2u, 3u };
    CHECK(h2_iostreamikcp_write(stream, payload, sizeof(payload)) == H2_PAL_ERR_TIMEOUT);
    h2_iostreamikcp_close(stream);
}

static void test_large_write_stops_io_after_first_timeout(void) {
    memory_io_t io = {0};
    memory_io_t ack = {0};
    h2_iostreamikcp_t *stream = open_stream_ex(&io, 8181u, 128u, 2048u, 0u);
    h2_iostreamikcp_t *peer = open_stream_ex(&ack, 8181u, 128u, 2048u, 0u);
    /* Grow beyond KCP's initial one-packet congestion window. */
    CHECK(h2_iostreamikcp_write(stream, (const uint8_t *)"warmup", 6u) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_input(peer, io.bytes, io.len) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_flush(peer) == H2_PAL_OK);
    CHECK(h2_iostreamikcp_input(stream, ack.bytes, ack.len) == H2_PAL_OK);
    io.write_result = H2_PAL_ERR_TIMEOUT;
    io.write_count = 0u;
    io.flush_count = 0u;
    uint8_t payload[8192] = {0};
    CHECK(h2_iostreamikcp_write(stream, payload, sizeof(payload)) == H2_PAL_ERR_TIMEOUT);
    CHECK(io.write_count == 1u);
    CHECK(io.flush_count == 0u);
    h2_iostreamikcp_close(peer);
    h2_iostreamikcp_close(stream);
}

static void test_flush_calls_transport_flush(void) {
    memory_io_t io = { 0 };
    h2_iostreamikcp_t *stream = open_stream(&io, 89u);
    CHECK(h2_iostreamikcp_flush(stream) == H2_PAL_OK);
    CHECK(io.flush_count == 1u);
    io.flush_result = H2_PAL_ERR_TIMEOUT;
    CHECK(h2_iostreamikcp_flush(stream) == H2_PAL_ERR_TIMEOUT);
    CHECK(io.flush_count == 2u);
    h2_iostreamikcp_close(stream);
}

static void test_timeout_and_empty_read(void) {
    memory_io_t io = { 0 };
    h2_iostreamikcp_t *stream = open_stream(&io, 77u);
    uint8_t out[4];
    size_t out_len = 99u;
    CHECK(h2_iostreamikcp_poll(stream, 1u) == H2_PAL_ERR_TIMEOUT);
    CHECK(h2_iostreamikcp_read(stream, out, sizeof(out), &out_len) == H2_PAL_ERR_WOULD_BLOCK);
    CHECK(out_len == 0u);
    h2_iostreamikcp_close(stream);
}

int main(void) {
    test_frame_filter_extracts_from_dirty_stream();
    test_filter_delivers_only_proven_log_bytes();
    test_session_control_frames();
    test_filter_rejects_invalid_control_frame();
    test_filter_discards_oversized_binary_candidate();
    test_filter_rejects_false_magic_and_crc();
    test_filter_resyncs_before_invalid_metadata_length();
    test_filter_multiple_frames();
    test_filter_continues_after_corrupt_frame();
    test_filter_resyncs_to_frame_inside_bad_crc_candidate();
    test_configured_receive_window_is_advertised();
    test_decoded_data_frame_input();
    test_kcp_reconstructs_stream();
    test_kcp_drops_malformed_frame_and_recovers();
    test_stats_include_filter_errors();
    test_kcp_chunks_large_writes_and_drains_after_read();
    test_write_reports_transport_failure();
    test_flush_calls_transport_flush();
    test_large_write_stops_io_after_first_timeout();
    test_timeout_and_empty_read();
    return 0;
}
