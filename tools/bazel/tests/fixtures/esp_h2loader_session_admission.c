#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "h2_iostreamikcp.h"

/* Only the native scheduler and transport boundary are faked. */
typedef unsigned TickType_t;
#define pdMS_TO_TICKS(ms) ((ms) / TICK_MS)
#define H2_LOADER_TRANSPORT_POLL_INTERVAL_MS 10u
#define H2_LOADER_APP_CLIENT_SESSION_RESET (-2)
typedef struct {
    h2_iostreamikcp_io_t physical_io;
    h2_iostreamikcp_t *stream;
    const h2_pal_mem_api_t *allocator;
    uint32_t conv, pending_conv, receive_window;
    int replacement_pending, close_pending;
} h2_esp_h2loader_command_transport_t;
typedef struct {
    h2_pal_result_t (*read)(void *, void *, size_t, size_t *, uint32_t);
} read_vtable_t;
typedef struct {
    h2_esp_h2loader_command_transport_t transport;
    struct { void *user; const read_vtable_t *vtable; } io;
} h2_esp_h2loader_app_iostreamikcp_t;
static unsigned opens, closes, acks, delays, polls;
static unsigned live;
static uint32_t peer_open;
static h2_pal_result_t open_result = H2_PAL_OK, ack_result = H2_PAL_OK;
static char handle;
static uint32_t transport_now_ms(void *user) { (void)user; return 0u; }
static void vTaskDelay(TickType_t ticks) {
    assert(ticks > 0u);
    assert(ticks == (10u / TICK_MS ? 10u / TICK_MS : 1u));
    ++delays;
}
h2_pal_result_t h2_iostreamikcp_open(const h2_iostreamikcp_config_t *config,
                                    h2_iostreamikcp_t **out) {
    assert(live == 0u);
    assert(config->conv != 0u);
    assert(config->mtu == H2_IOSTREAMIKCP_DEFAULT_MTU);
    assert(config->rx_buffer_size == 4096u && config->receive_window == 20u);
    ++opens;
    *out = NULL;
    if (open_result == H2_PAL_OK) {
        *out = (h2_iostreamikcp_t *)&handle;
        ++live;
    }
    return open_result;
}
void h2_iostreamikcp_close(h2_iostreamikcp_t *stream) {
    if (stream != NULL) {
        assert(stream == (h2_iostreamikcp_t *)&handle && live == 1u);
        --live;
        ++closes;
    }
}
static h2_pal_result_t transport_send_control(
    h2_esp_h2loader_command_transport_t *transport, uint8_t flags, uint32_t conv) {
    assert(flags == H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_ACK);
    assert(transport->conv == conv && live == 1u);
    ++acks;
    return ack_result;
}
static h2_pal_result_t h2_esp_h2loader_command_transport_poll_session(
    h2_esp_h2loader_command_transport_t *transport, uint32_t timeout_ms) {
    assert(timeout_ms == 50u);
    ++polls;
    transport->pending_conv = peer_open;
    peer_open = 0u;
    return transport->pending_conv ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}
static h2_pal_result_t read_byte(void *user, void *out, size_t len,
                               size_t *count, uint32_t timeout_ms) {
    (void)user;
    assert(len == 1u && timeout_ms == 50u);
    *(uint8_t *)out = 's';
    *count = 1u;
    return H2_PAL_OK;
}
/* FUNCTIONS */

static void assert_empty(const h2_esp_h2loader_command_transport_t *transport) {
    assert(transport->stream == NULL && transport->conv == 0u);
    assert(transport->pending_conv == 0u);
    assert(!transport->replacement_pending && !transport->close_pending);
    assert(live == 0u);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const read_vtable_t io = {.read = read_byte};
    h2_esp_h2loader_app_iostreamikcp_t state = {
        .transport = {.pending_conv = 7u, .receive_window = 20u},
        .io = {.vtable = &io},
    };
    if (strcmp(argv[1], "invalid") == 0) {
        assert(h2_esp_h2loader_command_transport_activate_pending(NULL) == H2_PAL_ERR_INVALID_STATE);
        state.transport.pending_conv = 0u;
        assert(h2_esp_h2loader_command_transport_activate_pending(&state.transport) == H2_PAL_ERR_INVALID_STATE);
        assert(opens == 0u && delays == 0u);
        return 0;
    }
    if (strcmp(argv[1], "success") != 0) {
        /* Replacing an existing session must release it and stale flags. */
        state.transport.stream = (h2_iostreamikcp_t *)&handle;
        state.transport.conv = 3u;
        state.transport.replacement_pending = state.transport.close_pending = 1;
        live = 1u;
        if (strcmp(argv[1], "ack") == 0) ack_result = H2_PAL_ERR_IO;
        else open_result = H2_PAL_ERR_NO_MEMORY;
        assert(app_read_byte(&state, 50u) == EOF);
        assert_empty(&state.transport);
        assert(opens == 1u && delays == 1u);
        assert(closes == (ack_result == H2_PAL_OK ? 1u : 2u));
        assert(acks == (ack_result == H2_PAL_OK ? 0u : 1u));
        for (unsigned i = 0u; i < 100u; ++i) {
            assert(app_read_byte(&state, 50u) == EOF);
        }
        assert(opens == 1u && polls == 100u && delays == 1u);
        if (strcmp(argv[1], "flood") == 0) {
            for (unsigned i = 0u; i < 100u; ++i) {
                peer_open = 7u;
                assert(app_read_byte(&state, 50u) == EOF);
                assert_empty(&state.transport);
            }
            assert(opens == 101u && delays == 101u && polls == 200u);
        }
        open_result = ack_result = H2_PAL_OK;
        /* Retransmission of the same OPEN, then a different peer, can recover. */
        peer_open = 7u;
    }
    unsigned previous_delays = delays;
    assert(app_read_byte(&state, 50u) == H2_LOADER_APP_CLIENT_SESSION_RESET);
    assert(state.transport.conv == 7u && state.transport.pending_conv == 0u);
    assert(app_read_byte(&state, 50u) == 's');
    state.transport.pending_conv = 9u;
    state.transport.replacement_pending = 1;
    assert(app_read_byte(&state, 50u) == H2_LOADER_APP_CLIENT_SESSION_RESET);
    assert(state.transport.conv == 9u && live == 1u);
    assert(delays == previous_delays);
    state.transport.close_pending = 1;
    assert(app_read_byte(&state, 50u) == EOF);
    assert_empty(&state.transport);
    return 0;
}
