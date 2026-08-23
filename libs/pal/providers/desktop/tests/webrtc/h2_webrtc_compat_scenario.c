#include "h2_webrtc_compat_scenario.h"

#include "h2_webrtc_compat_factory.h"
#include "h2_webrtc_pion_fixture.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    H2_WEBRTC_COMPAT_CHANNEL_COUNT = 3,
    H2_WEBRTC_COMPAT_REVERSE_CHANNEL_COUNT = 3,
    H2_WEBRTC_COMPAT_TOTAL_CHANNEL_COUNT = 6,
    H2_WEBRTC_COMPAT_MESSAGE_KIND_COUNT = 2,
    H2_WEBRTC_COMPAT_EVENT_COUNT = 64,
    H2_WEBRTC_COMPAT_EVENT_SIZE = 160,
    H2_WEBRTC_COMPAT_LOCAL_SID_COUNT = 150,
    H2_WEBRTC_COMPAT_REUSE_CYCLES = 512,
};

static volatile sig_atomic_t h2_webrtc_compat_fixture_pid;

static const char *const
    h2_webrtc_compat_labels[H2_WEBRTC_COMPAT_CHANNEL_COUNT] = {
        "giznet/v1/packet",
        "giznet/v1/service/32",
        "giznet/v1/service/0",
};

static const char *const
    h2_webrtc_compat_reverse_labels[H2_WEBRTC_COMPAT_REVERSE_CHANNEL_COUNT] = {
        "server/reverse/0",
        "server/reverse/1",
        "server/reverse/2",
};

typedef struct h2_webrtc_compat_state {
    char offer[16384];
    size_t offer_len;
    h2_pal_webrtc_peer_state_t peer_state;
    unsigned int channel_open_mask;
    unsigned int local_closed_mask;
    unsigned int echoed_mask;
    unsigned int reverse_request_mask;
    unsigned int reverse_closed_mask;
    uint16_t stream_ids[H2_WEBRTC_COMPAT_TOTAL_CHANNEL_COUNT];
    h2_pal_webrtc_channel_t
        *reverse_channels[H2_WEBRTC_COMPAT_REVERSE_CHANNEL_COUNT];
    const h2_pal_webrtc_api_t *api;
    int callback_error;
    int opus_echoed;
    size_t recycle_opened;
    size_t recycle_closed;
    size_t recycle_wait_opened;
    unsigned int recycle_echo_mask;
    uint8_t recycle_seen[H2_WEBRTC_COMPAT_LOCAL_SID_COUNT];
    size_t recycle_unique_sids;
    size_t recycle_reused_sids;
    int recycling;
    char recycle_expected[32];
    uint8_t recycle_binary[4];
    const char *backend_name;
    const char *mode;
    const char *phase;
    char events[H2_WEBRTC_COMPAT_EVENT_COUNT][H2_WEBRTC_COMPAT_EVENT_SIZE];
    size_t event_next;
    size_t event_count;
} h2_webrtc_compat_state_t;

static void h2_webrtc_compat_record(h2_webrtc_compat_state_t *state,
                                    const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vsnprintf(state->events[state->event_next],
                    H2_WEBRTC_COMPAT_EVENT_SIZE, format, args);
    va_end(args);
    state->event_next = (state->event_next + 1u) % H2_WEBRTC_COMPAT_EVENT_COUNT;
    if (state->event_count < H2_WEBRTC_COMPAT_EVENT_COUNT) {
        state->event_count++;
    }
}

static void h2_webrtc_compat_set_phase(h2_webrtc_compat_state_t *state,
                                       const char *phase) {
    state->phase = phase;
    h2_webrtc_compat_record(state, "phase=%s", phase);
}

static void
h2_webrtc_compat_dump_failure(const h2_webrtc_compat_state_t *state) {
    fprintf(stderr,
            "compat failure backend=%s mode=%s phase=%s peer_state=%d "
            "callback_error=%d\n",
            state->backend_name, state->mode, state->phase, state->peer_state,
            state->callback_error);
    size_t start = state->event_count == H2_WEBRTC_COMPAT_EVENT_COUNT
                       ? state->event_next
                       : 0u;
    for (size_t i = 0u; i < state->event_count; ++i) {
        size_t index = (start + i) % H2_WEBRTC_COMPAT_EVENT_COUNT;
        fprintf(stderr, "  event[%zu] %s\n", i, state->events[index]);
    }
}

static uint64_t h2_webrtc_compat_now_ms(void) {
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static int h2_webrtc_compat_channel_index(h2_pal_webrtc_str_t label) {
    for (int i = 0; i < H2_WEBRTC_COMPAT_CHANNEL_COUNT; ++i) {
        size_t expected_len = strlen(h2_webrtc_compat_labels[i]);
        if (label.len == expected_len &&
            memcmp(label.data, h2_webrtc_compat_labels[i], expected_len) == 0) {
            return i;
        }
    }
    for (int i = 0; i < H2_WEBRTC_COMPAT_REVERSE_CHANNEL_COUNT; ++i) {
        size_t expected_len = strlen(h2_webrtc_compat_reverse_labels[i]);
        if (label.len == expected_len &&
            memcmp(label.data, h2_webrtc_compat_reverse_labels[i],
                   expected_len) == 0) {
            return H2_WEBRTC_COMPAT_CHANNEL_COUNT + i;
        }
    }
    return -1;
}

static h2_pal_result_t h2_webrtc_compat_send(const h2_pal_webrtc_api_t *api,
                                             h2_pal_webrtc_peer_t *peer,
                                             h2_pal_webrtc_channel_t *channel,
                                             const uint8_t *data, size_t len,
                                             int is_text);

static void h2_webrtc_compat_on_peer_state(void *user,
                                           h2_pal_webrtc_peer_t *peer,
                                           h2_pal_webrtc_peer_state_t state) {
    (void)peer;
    h2_webrtc_compat_state_t *compat = (h2_webrtc_compat_state_t *)user;
    compat->peer_state = state;
    h2_webrtc_compat_record(compat, "peer_state=%d", state);
}

static void h2_webrtc_compat_on_local_sdp(void *user,
                                          h2_pal_webrtc_peer_t *peer,
                                          h2_pal_webrtc_sdp_type_t type,
                                          h2_pal_webrtc_str_t sdp) {
    (void)peer;
    h2_webrtc_compat_state_t *state = (h2_webrtc_compat_state_t *)user;
    if (type != H2_PAL_WEBRTC_SDP_OFFER || state->offer_len != 0u ||
        sdp.data == NULL || sdp.len == 0u || sdp.len >= sizeof(state->offer)) {
        state->callback_error = 1;
        return;
    }
    memcpy(state->offer, sdp.data, sdp.len);
    state->offer[sdp.len] = '\0';
    state->offer_len = sdp.len;
    h2_webrtc_compat_record(state, "local_sdp len=%zu", sdp.len);
}

static void
h2_webrtc_compat_on_channel_state(void *user, h2_pal_webrtc_peer_t *peer,
                                  h2_pal_webrtc_channel_t *channel,
                                  const h2_pal_webrtc_channel_info_t *info,
                                  h2_pal_webrtc_channel_state_t state_value) {
    (void)peer;
    h2_webrtc_compat_state_t *state = (h2_webrtc_compat_state_t *)user;
    int index = info == NULL ? -1 : h2_webrtc_compat_channel_index(info->label);
    h2_webrtc_compat_record(state, "channel_state index=%d state=%d stream=%u",
                            index, state_value,
                            info == NULL ? 0u : info->stream_id);
    if (index < 0 || !info->has_stream_id || info->ordered != (index != 0) ||
        info->reliable != (index != 0)) {
        state->callback_error = 1;
        return;
    }
    if (state->recycling && index == 1) {
        if (info->stream_id >= 300u || (info->stream_id & 1u) == 0u) {
            state->callback_error = 1;
            return;
        }
        if (state_value == H2_PAL_WEBRTC_CHANNEL_OPEN) {
            size_t slot = (size_t)((info->stream_id - 1u) / 2u);
            if (state->recycle_seen[slot]) {
                state->recycle_reused_sids++;
            } else {
                state->recycle_seen[slot] = 1u;
                state->recycle_unique_sids++;
            }
            state->recycle_opened++;
        } else if (state_value == H2_PAL_WEBRTC_CHANNEL_CLOSED) {
            state->recycle_closed++;
        } else if (state_value == H2_PAL_WEBRTC_CHANNEL_ERROR) {
            state->callback_error = 1;
        }
        return;
    }
    if (state_value == H2_PAL_WEBRTC_CHANNEL_OPEN) {
        int is_reverse = index >= H2_WEBRTC_COMPAT_CHANNEL_COUNT;
        if (info->stream_id >= 300u ||
            ((info->stream_id & 1u) == 0u) != is_reverse) {
            state->callback_error = 1;
            return;
        }
        for (int i = 0; i < H2_WEBRTC_COMPAT_TOTAL_CHANNEL_COUNT; ++i) {
            if (i != index && (state->channel_open_mask & (1u << i)) != 0u &&
                state->stream_ids[i] == info->stream_id) {
                state->callback_error = 1;
                return;
            }
        }
        state->stream_ids[index] = info->stream_id;
        state->channel_open_mask |= 1u << index;
        if (is_reverse) {
            int reverse_index = index - H2_WEBRTC_COMPAT_CHANNEL_COUNT;
            state->reverse_channels[reverse_index] = channel;
        }
    } else if (state_value == H2_PAL_WEBRTC_CHANNEL_CLOSED) {
        if (index >= H2_WEBRTC_COMPAT_CHANNEL_COUNT) {
            state->reverse_closed_mask |=
                1u << (index - H2_WEBRTC_COMPAT_CHANNEL_COUNT);
        } else {
            state->local_closed_mask |= 1u << index;
        }
    } else if (state_value == H2_PAL_WEBRTC_CHANNEL_ERROR) {
        state->callback_error = 1;
    }
}

static void h2_webrtc_compat_on_channel_message(
    void *user, h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_channel_t *channel,
    const h2_pal_webrtc_channel_info_t *info, const uint8_t *data, size_t len,
    int is_text) {
    (void)peer;
    (void)channel;
    h2_webrtc_compat_state_t *state = (h2_webrtc_compat_state_t *)user;
    int index = info == NULL ? -1 : h2_webrtc_compat_channel_index(info->label);
    h2_webrtc_compat_record(state, "channel_message index=%d len=%zu text=%d",
                            index, len, is_text);
    if (index < 0 || data == NULL) {
        state->callback_error = 1;
        return;
    }
    if (index >= H2_WEBRTC_COMPAT_CHANNEL_COUNT) {
        int reverse_index = index - H2_WEBRTC_COMPAT_CHANNEL_COUNT;
        char expected[40];
        int expected_len = snprintf(expected, sizeof(expected),
                                    "server-reverse-ack:%d", reverse_index);
        if (!is_text || expected_len <= 0 || (size_t)expected_len != len ||
            memcmp(data, expected, len) != 0) {
            state->callback_error = 1;
            return;
        }
        state->reverse_request_mask |= 1u << reverse_index;
        return;
    }
    if (state->recycling && index == 1) {
        static const char text_prefix[] = "server-service-ack-text:";
        static const char binary_prefix[] = "server-service-ack-binary:";
        size_t expected_len = strlen(state->recycle_expected);
        if (is_text && len == sizeof(text_prefix) - 1u + expected_len &&
            memcmp(data, text_prefix, sizeof(text_prefix) - 1u) == 0 &&
            memcmp(data + sizeof(text_prefix) - 1u, state->recycle_expected,
                   expected_len) == 0) {
            state->recycle_echo_mask |= 1u;
            return;
        }
        if (!is_text &&
            len == sizeof(binary_prefix) - 1u + sizeof(state->recycle_binary) &&
            memcmp(data, binary_prefix, sizeof(binary_prefix) - 1u) == 0 &&
            memcmp(data + sizeof(binary_prefix) - 1u, state->recycle_binary,
                   sizeof(state->recycle_binary)) == 0) {
            state->recycle_echo_mask |= 2u;
            return;
        }
        state->callback_error = 1;
        return;
    }
    static const char packet_text_prefix[] = "server-echo-text:text-";
    static const char packet_binary_prefix[] = "server-echo-binary:";
    static const char service_text_prefix[] = "server-service-ack-text:text-";
    static const char service_binary_prefix[] = "server-service-ack-binary:";
    const char *text_prefix =
        index == 0 ? packet_text_prefix : service_text_prefix;
    const char *binary_prefix =
        index == 0 ? packet_binary_prefix : service_binary_prefix;
    size_t text_prefix_len = strlen(text_prefix);
    size_t binary_prefix_len = strlen(binary_prefix);
    if (len == text_prefix_len + 1u &&
        memcmp(data, text_prefix, text_prefix_len) == 0 &&
        data[text_prefix_len] == (uint8_t)('0' + index)) {
        state->echoed_mask |= 1u
                              << (index * H2_WEBRTC_COMPAT_MESSAGE_KIND_COUNT);
        return;
    }
    static const uint8_t binary_tail[] = {0x00u, 0x80u, 0x00u, 0xffu};
    if (len == binary_prefix_len + sizeof(binary_tail) &&
        memcmp(data, binary_prefix, binary_prefix_len) == 0 &&
        data[binary_prefix_len] == binary_tail[0] &&
        data[binary_prefix_len + 1u] == binary_tail[1] &&
        data[binary_prefix_len + 2u] == (uint8_t)index &&
        data[binary_prefix_len + 3u] == binary_tail[3]) {
        state->echoed_mask |=
            1u << (index * H2_WEBRTC_COMPAT_MESSAGE_KIND_COUNT + 1);
        return;
    }
    state->callback_error = 1;
}

static void h2_webrtc_compat_on_opus(void *user, h2_pal_webrtc_peer_t *peer,
                                     const uint8_t *opus, size_t opus_len) {
    (void)peer;
    h2_webrtc_compat_state_t *state = (h2_webrtc_compat_state_t *)user;
    static const uint8_t expected[] = {0xf8u, 0x55u};
    h2_webrtc_compat_record(state, "opus len=%zu", opus_len);
    if (opus_len == sizeof(expected) && opus != NULL &&
        memcmp(opus, expected, sizeof(expected)) == 0) {
        state->opus_echoed = 1;
    } else {
        state->callback_error = 1;
    }
}

static int h2_webrtc_compat_wait(const h2_pal_webrtc_api_t *api,
                                 h2_pal_webrtc_peer_t *peer,
                                 h2_webrtc_compat_state_t *state,
                                 int (*done)(const h2_webrtc_compat_state_t *),
                                 uint32_t timeout_ms) {
    uint64_t deadline = h2_webrtc_compat_now_ms() + timeout_ms;
    while (!done(state) && !state->callback_error &&
           h2_webrtc_compat_now_ms() < deadline) {
        h2_pal_result_t result = h2_pal_webrtc_peer_poll(api, peer, 10);
        if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK &&
            result != H2_PAL_ERR_TIMEOUT && result != H2_PAL_ERR_CLOSED) {
            return -1;
        }
    }
    return done(state) && !state->callback_error ? 0 : -1;
}

static int h2_webrtc_compat_connected(const h2_webrtc_compat_state_t *state) {
    return state->peer_state == H2_PAL_WEBRTC_PEER_CONNECTED &&
           state->channel_open_mask ==
               (1u << H2_WEBRTC_COMPAT_TOTAL_CHANNEL_COUNT) - 1u;
}

static int
h2_webrtc_compat_reverse_acked(const h2_webrtc_compat_state_t *state) {
    return state->reverse_request_mask ==
           (1u << H2_WEBRTC_COMPAT_REVERSE_CHANNEL_COUNT) - 1u;
}

static int h2_webrtc_compat_echoed(const h2_webrtc_compat_state_t *state) {
    return state->echoed_mask == (1u << (H2_WEBRTC_COMPAT_CHANNEL_COUNT *
                                         H2_WEBRTC_COMPAT_MESSAGE_KIND_COUNT)) -
                                     1u;
}

static int h2_webrtc_compat_opus_echoed(const h2_webrtc_compat_state_t *state) {
    return state->opus_echoed;
}

static int
h2_webrtc_compat_recycle_opened(const h2_webrtc_compat_state_t *state) {
    return state->peer_state == H2_PAL_WEBRTC_PEER_CONNECTED &&
           state->recycle_opened >= state->recycle_wait_opened;
}

static int
h2_webrtc_compat_recycle_echoed(const h2_webrtc_compat_state_t *state) {
    return state->recycle_echo_mask == 3u;
}

static int h2_webrtc_compat_terminal(const h2_webrtc_compat_state_t *state) {
    return state->peer_state == H2_PAL_WEBRTC_PEER_DISCONNECTED ||
           ((state->peer_state == H2_PAL_WEBRTC_PEER_FAILED ||
             state->peer_state == H2_PAL_WEBRTC_PEER_CLOSED) &&
            state->reverse_closed_mask ==
                (1u << H2_WEBRTC_COMPAT_REVERSE_CHANNEL_COUNT) - 1u);
}

static h2_pal_result_t h2_webrtc_compat_send(const h2_pal_webrtc_api_t *api,
                                             h2_pal_webrtc_peer_t *peer,
                                             h2_pal_webrtc_channel_t *channel,
                                             const uint8_t *data, size_t len,
                                             int is_text) {
    uint64_t deadline = h2_webrtc_compat_now_ms() + 10000u;
    do {
        h2_pal_result_t result =
            h2_pal_webrtc_channel_send(api, channel, data, len, is_text);
        if (result != H2_PAL_ERR_WOULD_BLOCK) {
            return result;
        }
        (void)h2_pal_webrtc_peer_poll(api, peer, 10);
    } while (h2_webrtc_compat_now_ms() < deadline);
    return H2_PAL_ERR_TIMEOUT;
}

static int
h2_webrtc_compat_wait_reverse_replies(const h2_webrtc_compat_backend_t *backend,
                                      h2_webrtc_pion_fixture_t *fixture,
                                      h2_pal_webrtc_peer_t *peer) {
    uint64_t deadline = h2_webrtc_compat_now_ms() + 10000u;
    do {
        h2_webrtc_channel_stats_t stats = {0};
        if (h2_webrtc_pion_fixture_channel_stats(fixture, &stats) == 0 &&
            stats.reverse_replies ==
                (1u << H2_WEBRTC_COMPAT_REVERSE_CHANNEL_COUNT) - 1u) {
            return 0;
        }
        h2_pal_result_t result =
            h2_pal_webrtc_peer_poll(backend->api, peer, 10);
        if (result != H2_PAL_OK && result != H2_PAL_ERR_WOULD_BLOCK &&
            result != H2_PAL_ERR_TIMEOUT) {
            return -1;
        }
    } while (h2_webrtc_compat_now_ms() < deadline);
    return -1;
}

static int h2_webrtc_compat_run_reuse_cycles(
    const h2_webrtc_compat_backend_t *backend,
    h2_webrtc_pion_fixture_t *fixture, h2_pal_webrtc_peer_t *peer,
    h2_webrtc_compat_state_t *state, h2_pal_webrtc_channel_t **channels) {
    h2_webrtc_channel_stats_t baseline = {0};
    uint64_t deadline = h2_webrtc_compat_now_ms() + 10000u;
    do {
        (void)h2_pal_webrtc_peer_poll(backend->api, peer, 10);
        if (h2_webrtc_pion_fixture_channel_stats(fixture, &baseline) == 0 &&
            baseline.created == H2_WEBRTC_COMPAT_TOTAL_CHANNEL_COUNT &&
            baseline.opened == H2_WEBRTC_COMPAT_TOTAL_CHANNEL_COUNT &&
            baseline.current == H2_WEBRTC_COMPAT_TOTAL_CHANNEL_COUNT - 1u &&
            (state->local_closed_mask & (1u << 2u)) != 0u) {
            break;
        }
    } while (h2_webrtc_compat_now_ms() < deadline);
    if (baseline.opened != H2_WEBRTC_COMPAT_TOTAL_CHANNEL_COUNT ||
        baseline.current != H2_WEBRTC_COMPAT_TOTAL_CHANNEL_COUNT - 1u ||
        (state->local_closed_mask & (1u << 2u)) == 0u) {
        fprintf(stderr,
                "%s: rpc close baseline created=%llu opened=%llu closed=%llu "
                "current=%llu local_closed=0x%x\n",
                backend->name, baseline.created, baseline.opened,
                baseline.closed, baseline.current, state->local_closed_mask);
        return -1;
    }

    h2_pal_webrtc_channel_close(backend->api, channels[1]);
    channels[1] = NULL;
    channels[2] = NULL;
    deadline = h2_webrtc_compat_now_ms() + 10000u;
    h2_webrtc_channel_stats_t stats = {0};
    do {
        (void)h2_pal_webrtc_peer_poll(backend->api, peer, 10);
        if (h2_webrtc_pion_fixture_channel_stats(fixture, &stats) == 0 &&
            stats.closed >= baseline.closed + 1u &&
            stats.current == H2_WEBRTC_COMPAT_REVERSE_CHANNEL_COUNT + 1u) {
            break;
        }
    } while (h2_webrtc_compat_now_ms() < deadline);
    if (stats.closed < baseline.closed + 1u ||
        stats.current != H2_WEBRTC_COMPAT_REVERSE_CHANNEL_COUNT + 1u) {
        return -1;
    }

    state->recycling = 1;
    h2_pal_webrtc_channel_config_t config = {
        .label =
            {
                .data = h2_webrtc_compat_labels[1],
                .len = strlen(h2_webrtc_compat_labels[1]),
            },
        .ordered = 1,
        .reliable = 1,
    };
    for (size_t cycle = 0u; cycle < H2_WEBRTC_COMPAT_REUSE_CYCLES; ++cycle) {
        h2_pal_webrtc_channel_t *channel = NULL;
        h2_pal_result_t result = h2_pal_webrtc_peer_create_data_channel(
            backend->api, peer, &config, &channel);
        if (result != H2_PAL_OK) {
            fprintf(stderr, "%s: reuse cycle %zu create failed %d\n",
                    backend->name, cycle, result);
            return -1;
        }
        state->recycle_wait_opened = cycle + 1u;
        if (h2_webrtc_compat_wait(backend->api, peer, state,
                                  h2_webrtc_compat_recycle_opened,
                                  10000u) != 0) {
            fprintf(stderr, "%s: reuse cycle %zu did not open\n", backend->name,
                    cycle);
            return -1;
        }
        int payload_len =
            snprintf(state->recycle_expected, sizeof(state->recycle_expected),
                     "cycle-%zu", cycle);
        state->recycle_binary[0] = (uint8_t)(cycle >> 8u);
        state->recycle_binary[1] = (uint8_t)cycle;
        state->recycle_binary[2] = 0x80u;
        state->recycle_binary[3] = 0xffu;
        state->recycle_echo_mask = 0u;
        if (payload_len <= 0 ||
            (size_t)payload_len >= sizeof(state->recycle_expected) ||
            h2_webrtc_compat_send(backend->api, peer, channel,
                                  (const uint8_t *)state->recycle_expected,
                                  (size_t)payload_len, 1) != H2_PAL_OK) {
            fprintf(stderr, "%s: reuse cycle %zu send failed\n", backend->name,
                    cycle);
            return -1;
        }
        if (h2_webrtc_compat_send(
                backend->api, peer, channel, state->recycle_binary,
                sizeof(state->recycle_binary), 0) != H2_PAL_OK) {
            fprintf(stderr, "%s: reuse cycle %zu binary send failed\n",
                    backend->name, cycle);
            return -1;
        }
        if (h2_webrtc_compat_wait(backend->api, peer, state,
                                  h2_webrtc_compat_recycle_echoed,
                                  10000u) != 0) {
            fprintf(stderr, "%s: reuse cycle %zu echo failed\n", backend->name,
                    cycle);
            return -1;
        }
        h2_pal_webrtc_channel_close(backend->api, channel);
        if (state->recycle_closed != cycle + 1u) {
            fprintf(stderr, "%s: reuse cycle %zu close callback missing\n",
                    backend->name, cycle);
            return -1;
        }
        deadline = h2_webrtc_compat_now_ms() + 10000u;
        do {
            (void)h2_pal_webrtc_peer_poll(backend->api, peer, 10);
            if (h2_webrtc_pion_fixture_channel_stats(fixture, &stats) == 0 &&
                stats.closed >= baseline.closed + 2u + cycle &&
                stats.current == H2_WEBRTC_COMPAT_REVERSE_CHANNEL_COUNT + 1u) {
                break;
            }
        } while (h2_webrtc_compat_now_ms() < deadline);
        if (stats.closed < baseline.closed + 2u + cycle ||
            stats.current != H2_WEBRTC_COMPAT_REVERSE_CHANNEL_COUNT + 1u) {
            fprintf(stderr, "%s: reuse cycle %zu remote close timed out\n",
                    backend->name, cycle);
            return -1;
        }
    }
    if (stats.created != baseline.created + H2_WEBRTC_COMPAT_REUSE_CYCLES ||
        stats.opened != baseline.opened + H2_WEBRTC_COMPAT_REUSE_CYCLES ||
        stats.closed != baseline.closed + H2_WEBRTC_COMPAT_REUSE_CYCLES + 1u ||
        stats.current != H2_WEBRTC_COMPAT_REVERSE_CHANNEL_COUNT + 1u ||
        stats.max_current != H2_WEBRTC_COMPAT_TOTAL_CHANNEL_COUNT ||
        state->recycle_unique_sids > H2_WEBRTC_COMPAT_LOCAL_SID_COUNT ||
        state->recycle_reused_sids == 0u) {
        fprintf(stderr,
                "%s: incomplete reuse evidence created=%llu opened=%llu "
                "closed=%llu current=%llu max=%llu unique=%zu reused=%zu\n",
                backend->name, stats.created, stats.opened, stats.closed,
                stats.current, stats.max_current, state->recycle_unique_sids,
                state->recycle_reused_sids);
        return -1;
    }

    state->recycling = 0;
    state->echoed_mask &= ~3u;
    char text_payload[] = "text-0";
    static const uint8_t binary_payload[] = {0x00u, 0x80u, 0x00u, 0xffu};
    if (h2_webrtc_compat_send(backend->api, peer, channels[0],
                              (const uint8_t *)text_payload,
                              strlen(text_payload), 1) != H2_PAL_OK ||
        h2_webrtc_compat_send(backend->api, peer, channels[0], binary_payload,
                              sizeof(binary_payload), 0) != H2_PAL_OK ||
        h2_webrtc_compat_wait(backend->api, peer, state,
                              h2_webrtc_compat_echoed, 10000u) != 0) {
        return -1;
    }
    state->opus_echoed = 0;
    static const uint8_t opus[] = {0xf8u, 0x55u};
    if (h2_pal_webrtc_peer_send_opus(backend->api, peer, opus, sizeof(opus)) !=
            H2_PAL_OK ||
        h2_webrtc_compat_wait(backend->api, peer, state,
                              h2_webrtc_compat_opus_echoed, 10000u) != 0) {
        return -1;
    }
    h2_webrtc_compat_record(
        state, "reuse_complete cycles=%u unique=%zu reused=%zu",
        H2_WEBRTC_COMPAT_REUSE_CYCLES, state->recycle_unique_sids,
        state->recycle_reused_sids);
    return 0;
}

static int h2_webrtc_compat_run_session(
    const h2_webrtc_compat_backend_t *backend,
    h2_webrtc_pion_fixture_t *fixture, int relay_only, int require_remote_close,
    int run_reuse, const char *expected_protocol, int require_udp_drop) {
    int failed = 1;
    h2_pal_webrtc_peer_t *peer = NULL;
    h2_webrtc_compat_state_t state = {
        .backend_name = backend->name,
        .mode = relay_only ? "turn-relay-only" : fixture->mode,
        .phase = "peer-create",
        .api = backend->api,
    };
    h2_webrtc_compat_record(&state, "phase=peer-create");
    h2_pal_webrtc_callbacks_t callbacks = {
        .user = &state,
        .on_peer_state = h2_webrtc_compat_on_peer_state,
        .on_local_sdp = h2_webrtc_compat_on_local_sdp,
        .on_channel_state = h2_webrtc_compat_on_channel_state,
        .on_channel_message = h2_webrtc_compat_on_channel_message,
        .on_opus_frame = h2_webrtc_compat_on_opus,
    };
    h2_pal_result_t result =
        h2_pal_webrtc_peer_create(backend->api, &callbacks, &peer);
    if (result != H2_PAL_OK) {
        fprintf(stderr, "%s: peer create failed %d\n", backend->name, result);
        h2_webrtc_compat_record(&state, "peer_create result=%d", result);
        h2_webrtc_compat_dump_failure(&state);
        return 1;
    }
    h2_webrtc_compat_set_phase(&state, "channel-create");
    h2_pal_webrtc_channel_t *channels[H2_WEBRTC_COMPAT_CHANNEL_COUNT] = {0};
    for (size_t i = 0u; i < H2_WEBRTC_COMPAT_CHANNEL_COUNT; ++i) {
        h2_pal_webrtc_channel_config_t config = {
            .label =
                {
                    .data = h2_webrtc_compat_labels[i],
                    .len = strlen(h2_webrtc_compat_labels[i]),
                },
            .ordered = i != 0u,
            .reliable = i != 0u,
        };
        result = h2_pal_webrtc_peer_create_data_channel(backend->api, peer,
                                                        &config, &channels[i]);
        if (result != H2_PAL_OK) {
            fprintf(stderr, "%s: channel %zu failed %d\n", backend->name, i,
                    result);
            goto cleanup;
        }
    }
    char ice_url[96];
    int ice_url_len = snprintf(
        ice_url, sizeof(ice_url),
        relay_only ? "turn:127.0.0.1:%d?transport=udp" : "stun:127.0.0.1:%d",
        relay_only ? fixture->turn_port : fixture->stun_port);
    if (ice_url_len <= 0 || (size_t)ice_url_len >= sizeof(ice_url)) {
        fprintf(stderr, "%s: invalid ICE endpoint\n", backend->name);
        goto cleanup;
    }
    h2_pal_webrtc_ice_server_t ice_server = {
        .url = {.data = ice_url, .len = (size_t)ice_url_len},
        .username =
            {
                .data = relay_only ? fixture->turn_username : NULL,
                .len = relay_only ? strlen(fixture->turn_username) : 0u,
            },
        .credential =
            {
                .data = relay_only ? fixture->turn_credential : NULL,
                .len = relay_only ? strlen(fixture->turn_credential) : 0u,
            },
    };
    result = h2_pal_webrtc_peer_add_ice_server(backend->api, peer, &ice_server);
    if (result != H2_PAL_OK) {
        fprintf(stderr, "%s: add ICE failed %d\n", backend->name, result);
        goto cleanup;
    }
    h2_webrtc_compat_set_phase(&state, "offer");
    result = h2_pal_webrtc_peer_start_offer(backend->api, peer);
    if (result != H2_PAL_OK || state.offer_len == 0u) {
        fprintf(stderr, "%s: offer failed %d len=%zu\n", backend->name, result,
                state.offer_len);
        goto cleanup;
    }
    if (backend->supports_ice_tcp &&
        (strstr(state.offer, " TCP ") == NULL ||
         strstr(state.offer, " 9 typ host tcptype active") == NULL)) {
        fprintf(stderr, "%s: active TCP candidate missing from offer\n",
                backend->name);
        goto cleanup;
    }
    if (h2_pal_webrtc_peer_add_ice_server(backend->api, peer, &ice_server) !=
        H2_PAL_ERR_INVALID_STATE) {
        fprintf(stderr, "%s: late ICE server accepted\n", backend->name);
        goto cleanup;
    }
    char answer[16384];
    size_t answer_len = 0u;
    h2_webrtc_compat_set_phase(&state, "signaling");
    if (h2_webrtc_pion_fixture_exchange(
            fixture,
            (h2_pal_webrtc_str_t){.data = state.offer, .len = state.offer_len},
            answer, sizeof(answer), &answer_len, relay_only) != 0) {
        fprintf(stderr, "%s: SDP exchange failed\n", backend->name);
        goto cleanup;
    }
    int answer_has_udp =
        strstr(answer, " udp ") != NULL || strstr(answer, " UDP ") != NULL;
    int answer_has_tcp =
        strstr(answer, " tcp ") != NULL || strstr(answer, " TCP ") != NULL;
    int expect_udp_candidate = strcmp(fixture->mode, "tcp") != 0;
    int expect_tcp_candidate = strcmp(fixture->mode, "udp") != 0;
    if ((!relay_only && (answer_has_udp != expect_udp_candidate ||
                         answer_has_tcp != expect_tcp_candidate)) ||
        (relay_only && !answer_has_udp)) {
        fprintf(stderr, "%s: answer transport mismatch mode=%s udp=%d tcp=%d\n",
                backend->name, fixture->mode, answer_has_udp, answer_has_tcp);
        goto cleanup;
    }
    result = h2_pal_webrtc_peer_set_remote_sdp(
        backend->api, peer, H2_PAL_WEBRTC_SDP_ANSWER,
        (h2_pal_webrtc_str_t){.data = answer, .len = answer_len});
    h2_webrtc_compat_set_phase(&state, "connect");
    if (result != H2_PAL_OK ||
        h2_webrtc_compat_wait(backend->api, peer, &state,
                              h2_webrtc_compat_connected, 20000u) != 0) {
        fprintf(stderr,
                "%s: connect failed result=%d state=%d channels=0x%x "
                "callback_error=%d\n",
                backend->name, result, state.peer_state,
                state.channel_open_mask, state.callback_error);
        goto cleanup;
    }
    h2_webrtc_compat_set_phase(&state, "reverse-channel-response");
    for (int i = 0; i < H2_WEBRTC_COMPAT_REVERSE_CHANNEL_COUNT; ++i) {
        char probe[40];
        int probe_len =
            snprintf(probe, sizeof(probe), "client-reverse-probe:%d", i);
        h2_pal_result_t send_result =
            probe_len <= 0 || state.reverse_channels[i] == NULL
                ? H2_PAL_ERR_INVALID_STATE
                : h2_webrtc_compat_send(
                      backend->api, peer, state.reverse_channels[i],
                      (const uint8_t *)probe, (size_t)probe_len, 1);
        if (send_result != H2_PAL_OK) {
            fprintf(stderr, "%s: reverse channel %d send failed result=%d\n",
                    backend->name, i, send_result);
            goto cleanup;
        }
    }
    if (h2_webrtc_compat_wait(backend->api, peer, &state,
                              h2_webrtc_compat_reverse_acked, 10000u) != 0) {
        fprintf(stderr, "%s: reverse channel acknowledgements not observed\n",
                backend->name);
        goto cleanup;
    }
    if (h2_webrtc_compat_wait_reverse_replies(backend, fixture, peer) != 0) {
        fprintf(stderr, "%s: reverse channel replies not observed\n",
                backend->name);
        goto cleanup;
    }
    h2_webrtc_ice_pair_t pair = {0};
    if (h2_webrtc_pion_fixture_ice_pair(fixture, &pair) != 0 ||
        strcmp(pair.mode, fixture->mode) != 0 ||
        strcmp(pair.local_protocol, expected_protocol) != 0 ||
        strcmp(pair.remote_protocol, expected_protocol) != 0 ||
        pair.local_type[0] == '\0' || pair.remote_type[0] == '\0' ||
        (require_udp_drop && pair.udp_drops == 0u) ||
        (strcmp(expected_protocol, "tcp") == 0 &&
         (strcmp(pair.local_tcp_type, "passive") != 0 ||
          (pair.remote_tcp_type[0] != '\0' &&
           strcmp(pair.remote_tcp_type, "active") != 0)))) {
        fprintf(stderr,
                "%s: selected pair mismatch mode=%s local=%s/%s remote=%s/%s "
                "drops=%llu expected=%s\n",
                backend->name, pair.mode, pair.local_protocol,
                pair.local_tcp_type, pair.remote_protocol, pair.remote_tcp_type,
                pair.udp_drops, expected_protocol);
        goto cleanup;
    }
    fprintf(stderr,
            "%s: ICE evidence mode=%s answer_udp=%d answer_tcp=%d selected=%s "
            "drops=%llu\n",
            backend->name, fixture->mode, answer_has_udp, answer_has_tcp,
            expected_protocol, pair.udp_drops);
    for (size_t i = 0u; i < H2_WEBRTC_COMPAT_CHANNEL_COUNT; ++i) {
        h2_webrtc_compat_set_phase(&state, "data-send");
        char text_payload[] = "text-0";
        text_payload[5] = (char)('0' + i);
        static const uint8_t binary_template[] = {0x00u, 0x80u, 0x00u, 0xffu};
        uint8_t binary_payload[sizeof(binary_template)];
        memcpy(binary_payload, binary_template, sizeof(binary_payload));
        binary_payload[2] = (uint8_t)i;
        result = h2_webrtc_compat_send(backend->api, peer, channels[i],
                                       (const uint8_t *)text_payload,
                                       strlen(text_payload), 1);
        if (result == H2_PAL_OK) {
            result = h2_webrtc_compat_send(backend->api, peer, channels[i],
                                           binary_payload,
                                           sizeof(binary_payload), 0);
        }
        if (result != H2_PAL_OK) {
            fprintf(stderr, "%s: send %zu failed %d\n", backend->name, i,
                    result);
            goto cleanup;
        }
    }
    if (h2_webrtc_compat_wait(backend->api, peer, &state,
                              h2_webrtc_compat_echoed, 10000u) != 0) {
        fprintf(stderr, "%s: echo failed mask=0x%x callback_error=%d\n",
                backend->name, state.echoed_mask, state.callback_error);
        goto cleanup;
    }
    h2_webrtc_compat_set_phase(&state, "opus");
    static const uint8_t opus[] = {0xf8u, 0x55u};
    result =
        h2_pal_webrtc_peer_send_opus(backend->api, peer, opus, sizeof(opus));
    if (result != H2_PAL_OK ||
        h2_webrtc_compat_wait(backend->api, peer, &state,
                              h2_webrtc_compat_opus_echoed, 10000u) != 0) {
        fprintf(stderr, "%s: Opus echo failed result=%d\n", backend->name,
                result);
        goto cleanup;
    }
    if (run_reuse) {
        h2_webrtc_compat_set_phase(&state, "sid-reuse");
        if (h2_webrtc_compat_run_reuse_cycles(backend, fixture, peer, &state,
                                              channels) != 0) {
            goto cleanup;
        }
    }
    if (require_remote_close) {
        h2_webrtc_compat_set_phase(&state, "remote-close");
        if (h2_webrtc_pion_fixture_close_session(fixture) != 0 ||
            h2_webrtc_compat_wait(backend->api, peer, &state,
                                  h2_webrtc_compat_terminal, 10000u) != 0) {
            fprintf(stderr, "%s: remote close not observed state=%d\n",
                    backend->name, state.peer_state);
            goto cleanup;
        }
    }
    fprintf(stderr,
            "%s: session mode=%s data=pass opus=pass remote_close=%s "
            "sid_reuse=%s\n",
            backend->name, state.mode, require_remote_close ? "pass" : "skip",
            run_reuse ? "pass" : "skip");
    failed = 0;

cleanup:
    if (failed) {
        h2_webrtc_compat_dump_failure(&state);
    }
    if (peer != NULL) {
        h2_pal_webrtc_peer_close(backend->api, peer);
    }
    return failed;
}

static void h2_webrtc_compat_watchdog(int signal_number) {
    (void)signal_number;
    static const char message[] = "WebRTC compatibility watchdog expired\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1u);
    pid_t fixture_pid = (pid_t)h2_webrtc_compat_fixture_pid;
    if (fixture_pid > 0) {
        (void)kill(fixture_pid, SIGKILL);
        (void)waitpid(fixture_pid, NULL, 0);
    }
    _exit(124);
}

int h2_webrtc_compat_run(const char *server_path) {
    (void)signal(SIGALRM, h2_webrtc_compat_watchdog);
    alarm(180u);
    int failed = 1;
    h2_webrtc_compat_backend_t backend = {0};
    h2_pal_result_t result = h2_webrtc_compat_backend_create(&backend);
    if (result != H2_PAL_OK) {
        fprintf(stderr, "backend: create failed %d\n", result);
        goto cleanup;
    }
    const char *modes[] = {"udp", "tcp"};
    size_t mode_count =
        backend.supports_ice_tcp ? sizeof(modes) / sizeof(modes[0]) : 1u;
    for (size_t mode_index = 0u; mode_index < mode_count; ++mode_index) {
        const char *mode = modes[mode_index];
        const char *expected_protocol =
            strcmp(mode, "tcp") == 0 || strcmp(mode, "mixed-drop-udp") == 0
                ? "tcp"
                : "udp";
        int run_reuse = 0;
        h2_webrtc_pion_fixture_t fixture = {0};
        if (h2_webrtc_pion_fixture_start(&fixture, server_path, mode) != 0) {
            fprintf(stderr, "fixture: failed to start mode=%s\n", mode);
            goto cleanup;
        }
        h2_webrtc_compat_fixture_pid = (sig_atomic_t)fixture.pid;
        if (h2_webrtc_compat_run_session(
                &backend, &fixture, 0, 1, run_reuse, expected_protocol,
                strcmp(mode, "mixed-drop-udp") == 0) != 0 ||
            h2_webrtc_compat_run_session(
                &backend, &fixture, 0, 0, 0, expected_protocol,
                strcmp(mode, "mixed-drop-udp") == 0) != 0) {
            h2_webrtc_pion_fixture_stop(&fixture);
            h2_webrtc_compat_fixture_pid = 0;
            goto cleanup;
        }
        fprintf(stderr, "%s: mode=%s reconnect=pass\n", backend.name, mode);
        if (backend.supports_turn && strcmp(mode, "udp") == 0) {
            if (h2_webrtc_compat_run_session(&backend, &fixture, 1, 1, 0, "udp",
                                             0) != 0) {
                h2_webrtc_pion_fixture_stop(&fixture);
                h2_webrtc_compat_fixture_pid = 0;
                goto cleanup;
            }
            h2_webrtc_turn_stats_t stats = {0};
            if (h2_webrtc_pion_fixture_turn_stats(&fixture, &stats) != 0 ||
                stats.allocations_created == 0u ||
                stats.allocations_deleted == 0u ||
                stats.permissions_created == 0u || stats.relay_ingress == 0u ||
                stats.relay_egress == 0u) {
                fprintf(stderr,
                        "%s: incomplete TURN evidence alloc=%llu delete=%llu "
                        "permission=%llu ingress=%llu egress=%llu\n",
                        backend.name, stats.allocations_created,
                        stats.allocations_deleted, stats.permissions_created,
                        stats.relay_ingress, stats.relay_egress);
                h2_webrtc_pion_fixture_stop(&fixture);
                h2_webrtc_compat_fixture_pid = 0;
                goto cleanup;
            }
            fprintf(stderr,
                    "%s: TURN evidence alloc=%llu delete=%llu permission=%llu "
                    "ingress=%llu egress=%llu\n",
                    backend.name, stats.allocations_created,
                    stats.allocations_deleted, stats.permissions_created,
                    stats.relay_ingress, stats.relay_egress);
        }
        h2_webrtc_pion_fixture_stop(&fixture);
        h2_webrtc_compat_fixture_pid = 0;
    }
    failed = 0;

cleanup:
    if (backend.destroy != NULL) {
        backend.destroy(backend.state);
    }
    h2_webrtc_compat_fixture_pid = 0;
    alarm(0u);
    return failed;
}
