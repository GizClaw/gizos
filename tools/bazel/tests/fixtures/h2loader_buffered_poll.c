#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "h2_iostreamikcp.h"

/* FRAME */

typedef struct transport {
    h2_iostreamikcp_io_t physical_io;
    h2_iostreamikcp_filter_t filter;
    h2_iostreamikcp_t *stream;
} transport_t;
typedef transport_t h2_esp_h2loader_command_transport_t;
typedef transport_t h2_bk_serial_transport_t;
enum {
    H2_LOADER_TRANSPORT_PHYSICAL_READ_SIZE = 512,
    H2_LOADER_TRANSPORT_POLL_INTERVAL_MS = 10,
    H2_BK_SERIAL_PHYSICAL_READ_SIZE = 512,
    H2_BK_SERIAL_POLL_INTERVAL_MS = 10
};
#define transport_stop_requested(transport) 0
#define transport_now_ms(user) 0u
#define transport_on_frame on_frame
static uint8_t input[512];
static size_t input_size;
static int callback_timeout;
static int opens;
static int data_frames;
static int updates;
static h2_pal_result_t empty_result = H2_PAL_ERR_TIMEOUT;
static h2_pal_result_t read_result = H2_PAL_OK;

static h2_pal_result_t read_physical(void *user, void *buffer, size_t size,
                                    size_t *count, uint32_t timeout_ms) {
    (void)user;
    assert(timeout_ms <= 10u);
    *count = 0u;
    if (read_result != H2_PAL_OK)
        return read_result;
    if (input_size == 0u)
        return empty_result;
    assert(input_size <= size);
    memcpy(buffer, input, input_size);
    *count = input_size;
    input_size = 0u;
    return H2_PAL_OK;
}

static h2_pal_result_t on_frame(void *user, const h2_iostreamikcp_frame_t *frame) {
    (void)user;
    if (frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN) {
        ++opens;
        if (callback_timeout) {
            callback_timeout = 0;
            return H2_PAL_ERR_TIMEOUT;
        }
    } else {
        assert(frame->flags == H2_IOSTREAMIKCP_FRAME_FLAG_DATA);
        assert(frame->payload_len == 7u);
        assert(memcmp(frame->payload, "status\n", 7u) == 0);
        ++data_frames;
    }
    return H2_PAL_OK;
}

h2_pal_result_t h2_iostreamikcp_update(h2_iostreamikcp_t *stream, uint32_t now) {
    (void)now;
    assert(stream != NULL);
    ++updates;
    return H2_PAL_OK;
}

/* POLL */

static void append(uint8_t flags) {
    const uint8_t control[] = {9u, 0u, 0u, 0u};
    h2_iostreamikcp_frame_t frame = {
        .flags = flags,
        .conv = 9u,
        .payload = flags == H2_IOSTREAMIKCP_FRAME_FLAG_DATA
            ? (const uint8_t *)"status\n" : control,
        .payload_len = flags == H2_IOSTREAMIKCP_FRAME_FLAG_DATA ? 7u : 4u,
    };
    size_t size = 0u;
    assert(h2_iostreamikcp_frame_encode(&frame, input + input_size,
        sizeof(input) - input_size, &size) == H2_PAL_OK);
    input_size += size;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    transport_t transport = {
        .physical_io = {.read = read_physical},
        .stream = (h2_iostreamikcp_t *)&transport,
    };
    h2_iostreamikcp_filter_init(&transport.filter);
    if (strcmp(argv[1], "partial") == 0) {
        append(H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN);
        uint8_t suffix[22];
        size_t suffix_size = input_size - 13u;
        memcpy(suffix, input + 13u, suffix_size);
        input_size = 13u;
        assert(poll_physical(&transport, 50u) == H2_PAL_OK);
        assert(poll_physical(&transport, 50u) == H2_PAL_ERR_TIMEOUT);
        assert(opens == 0 && transport.filter.len == 13u);
        memcpy(input, suffix, suffix_size);
        input_size = suffix_size;
        assert(poll_physical(&transport, 50u) == H2_PAL_OK);
        assert(opens == 1 && transport.filter.len == 0u);
        return 0;
    }
    if (strcmp(argv[1], "would_block") == 0)
        empty_result = H2_PAL_ERR_WOULD_BLOCK;
    for (unsigned i = 0u; i < 20u; ++i) append(H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN);
    append(H2_IOSTREAMIKCP_FRAME_FLAG_DATA);
    callback_timeout = 1;
    assert(poll_physical(&transport, 50u) == H2_PAL_ERR_TIMEOUT);
    assert(opens == 1 && data_frames == 0 && transport.filter.len != 0u);
    if (strcmp(argv[1], "read_error") == 0) {
        read_result = H2_PAL_ERR_IO;
        assert(poll_physical(&transport, 50u) == H2_PAL_ERR_IO);
        assert(opens == 1 && data_frames == 0);
        read_result = H2_PAL_OK;
    }
    /* The UART is now idle: buffered complete frames need no new bytes. */
    assert(poll_physical(&transport, 50u) == empty_result);
    assert(opens == 20 && data_frames == 1 && transport.filter.len == 0u);
    assert(updates == 1);
    return 0;
}
