#include "h2/pal/h2_pal_unsupported.h"
#include "h2_peer_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_mem {
    size_t allocations;
    size_t frees;
    size_t fail_at;
    uint64_t now_ms;
} test_mem_t;

typedef enum test_channel_open_reentry {
    TEST_CHANNEL_OPEN_REENTRY_NONE = 0,
    TEST_CHANNEL_OPEN_REENTRY_TERMINAL,
    TEST_CHANNEL_OPEN_REENTRY_PEER_CLOSE,
    TEST_CHANNEL_OPEN_REENTRY_OWNER_DESTROY,
    TEST_CHANNEL_OPEN_REENTRY_REPLACE,
} test_channel_open_reentry_t;

typedef struct test_provider {
    size_t ice_open;
    size_t ice_close;
    size_t dtls_open;
    size_t dtls_close;
    size_t srtp_open;
    size_t srtp_close;
    size_t sctp_open;
    size_t sctp_close;
    size_t channel_open;
    size_t channel_close;
    uint16_t stream_ids[H2_PEER_LOCAL_STREAM_COUNT * 2u];
    size_t stream_count;
    uint16_t reset_stream_ids[H2_PEER_LOCAL_STREAM_COUNT];
    size_t reset_stream_count;
    h2_peer_sctp_event_t reset_events[16];
    size_t reset_event_count;
    h2_pal_result_t channel_close_result;
    size_t channel_send;
    uint16_t last_send_stream_id;
    uint8_t last_send[32];
    size_t last_send_len;
    int last_send_is_text;
    size_t srtp_send_calls;
    uint8_t last_rtp[1500];
    size_t last_rtp_len;
    int block_next_srtp_send;
    int fail_dtls_open;
    int fail_srtp_open;
    int emitted_open;
    h2_pal_result_t dtls_poll_result;
    test_mem_t *clock;
    int ice_timeout_ms;
    int dtls_timeout_ms;
    int sctp_timeout_ms;
    test_channel_open_reentry_t channel_open_reentry;
    h2_pal_result_t channel_open_result;
    const h2_pal_webrtc_api_t *reentry_api;
    h2_pal_webrtc_peer_t *reentry_peer;
    h2_peer_t **reentry_owner;
    h2_pal_result_t replacement_result;
    h2_pal_webrtc_channel_t *replacement_channel;
} test_provider_t;

typedef struct test_callbacks {
    size_t connecting;
    size_t connected;
    size_t disconnected;
    size_t failed;
    size_t closed;
    size_t channel_open;
    size_t channel_closed;
    size_t channel_error;
    size_t opus_frames;
    char local_sdp[4096];
    size_t local_sdp_len;
    const h2_pal_webrtc_api_t *api;
    h2_peer_t **owner;
    int close_on_connecting;
    int destroy_on_connecting;
    h2_pal_webrtc_channel_t *last_channel;
} test_callbacks_t;

static h2_pal_result_t test_ice_open(void *user,
                                     const h2_pal_webrtc_ice_server_t *servers,
                                     size_t server_count, void **out_session) {
    test_provider_t *provider = (test_provider_t *)user;
    provider->ice_open++;
    if (server_count != 0u) {
        assert(servers != NULL);
        assert(servers[0].url.len == sizeof("stun:example.invalid:3478") - 1u);
        assert(memcmp(servers[0].url.data, "stun:example.invalid:3478",
                      servers[0].url.len) == 0);
    }
    *out_session = provider;
    return H2_PAL_OK;
}

static h2_pal_result_t test_ice_poll(void *user, void *session,
                                     int timeout_ms) {
    test_provider_t *provider = (test_provider_t *)user;
    assert(provider == session);
    provider->ice_timeout_ms = timeout_ms;
    provider->clock->now_ms += 4u;
    return H2_PAL_OK;
}

static void test_ice_close(void *user, void *session) {
    test_provider_t *provider = (test_provider_t *)user;
    assert(session == provider);
    provider->ice_close++;
}

static void *test_alloc(void *user, size_t len) {
    test_mem_t *mem = (test_mem_t *)user;
    if (mem->fail_at != 0u && mem->allocations + 1u == mem->fail_at) {
        return NULL;
    }
    void *ptr = malloc(len);
    if (ptr != NULL) {
        mem->allocations++;
    }
    return ptr;
}

static void *test_realloc(void *user, void *ptr, size_t len) {
    (void)user;
    return realloc(ptr, len);
}

static void test_free(void *user, void *ptr) {
    test_mem_t *mem = (test_mem_t *)user;
    free(ptr);
    mem->frees++;
}

static h2_pal_result_t test_random(void *user, uint8_t *out, size_t len) {
    (void)user;
    for (size_t i = 0u; i < len; ++i) {
        out[i] = (uint8_t)(i + 1u);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t test_monotonic_ms(void *user, uint64_t *out_ms) {
    test_mem_t *mem = (test_mem_t *)user;
    *out_ms = mem->now_ms;
    return H2_PAL_OK;
}

static h2_pal_result_t test_monotonic_us(void *user, uint64_t *out_us) {
    uint64_t now_ms = 0u;
    h2_pal_result_t result = test_monotonic_ms(user, &now_ms);
    *out_us = now_ms * UINT64_C(1000);
    return result;
}

static h2_pal_result_t test_dtls_open(void *user, void **out_session) {
    test_provider_t *provider = (test_provider_t *)user;
    provider->dtls_open++;
    *out_session = NULL;
    if (provider->fail_dtls_open) {
        return H2_PAL_ERR_IO;
    }
    *out_session = provider;
    return H2_PAL_OK;
}

static h2_pal_result_t test_dtls_poll(void *user, void *session,
                                      int timeout_ms) {
    test_provider_t *provider = (test_provider_t *)user;
    assert(provider == session);
    provider->dtls_timeout_ms = timeout_ms;
    provider->clock->now_ms += 4u;
    return provider->dtls_poll_result;
}

static h2_pal_result_t test_dtls_fingerprint(void *user, void *session,
                                             char *out, size_t out_cap,
                                             size_t *out_len) {
    static const char fingerprint[] =
        "00:01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:"
        "10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F";
    assert(user == session);
    *out_len = 0u;
    if (out_cap < sizeof(fingerprint) - 1u) {
        return H2_PAL_ERR_NO_SPACE;
    }
    memcpy(out, fingerprint, sizeof(fingerprint) - 1u);
    *out_len = sizeof(fingerprint) - 1u;
    return H2_PAL_OK;
}

static h2_pal_result_t
test_dtls_set_remote_fingerprint(void *user, void *session,
                                 h2_pal_webrtc_str_t fingerprint) {
    assert(user == session);
    assert(fingerprint.data != NULL &&
           fingerprint.len > sizeof("sha-256 ") - 1u);
    assert(memcmp(fingerprint.data, "sha-256 ", sizeof("sha-256 ") - 1u) == 0);
    return H2_PAL_OK;
}

static void test_dtls_close(void *user, void *session) {
    test_provider_t *provider = (test_provider_t *)user;
    assert(session == provider);
    provider->dtls_close++;
}

static h2_pal_result_t test_srtp_open(void *user, void **out_session) {
    test_provider_t *provider = (test_provider_t *)user;
    provider->srtp_open++;
    *out_session = NULL;
    if (provider->fail_srtp_open) {
        return H2_PAL_ERR_IO;
    }
    *out_session = provider;
    return H2_PAL_OK;
}

static h2_pal_result_t test_srtp_send(void *user, void *session,
                                      const uint8_t *packet,
                                      size_t packet_len) {
    test_provider_t *provider = (test_provider_t *)user;
    assert(session == provider);
    provider->srtp_send_calls++;
    if (provider->block_next_srtp_send) {
        provider->block_next_srtp_send = 0;
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    assert(packet_len <= sizeof(provider->last_rtp));
    memcpy(provider->last_rtp, packet, packet_len);
    provider->last_rtp_len = packet_len;
    return H2_PAL_OK;
}

static h2_pal_result_t test_srtp_receive(void *user, void *session,
                                         const uint8_t *packet,
                                         size_t packet_len, uint8_t *out_packet,
                                         size_t out_cap, size_t *out_len) {
    assert(user == session);
    *out_len = 0u;
    if (packet_len > out_cap) {
        return H2_PAL_ERR_NO_SPACE;
    }
    memcpy(out_packet, packet, packet_len);
    *out_len = packet_len;
    return H2_PAL_OK;
}

static void test_srtp_close(void *user, void *session) {
    test_provider_t *provider = (test_provider_t *)user;
    assert(session == provider);
    provider->srtp_close++;
}

static h2_pal_result_t test_sctp_open(void *user, void **out_session) {
    test_provider_t *provider = (test_provider_t *)user;
    provider->sctp_open++;
    *out_session = provider;
    return H2_PAL_OK;
}

static h2_pal_result_t test_sctp_poll(void *user, void *session, int timeout_ms,
                                      h2_peer_sctp_event_fn event_fn,
                                      void *event_user) {
    test_provider_t *provider = (test_provider_t *)user;
    assert(session == provider);
    provider->sctp_timeout_ms = timeout_ms;
    if (!provider->emitted_open) {
        for (size_t i = 0u; i < provider->stream_count; ++i) {
            h2_peer_sctp_event_t event = {
                .type = H2_PEER_SCTP_EVENT_CHANNEL_OPEN,
                .stream_id = provider->stream_ids[i],
            };
            event_fn(event_user, &event);
        }
        provider->emitted_open = 1;
    }
    for (size_t i = 0u; i < provider->reset_event_count; ++i) {
        event_fn(event_user, &provider->reset_events[i]);
    }
    provider->reset_event_count = 0u;
    return H2_PAL_OK;
}

static h2_pal_result_t test_sctp_channel_open(void *user, void *session,
                                              uint16_t stream_id,
                                              h2_pal_webrtc_str_t label,
                                              int ordered, int reliable) {
    test_provider_t *provider = (test_provider_t *)user;
    assert(session == provider);
    assert(label.data != NULL && label.len != 0u);
    assert(ordered == 0 || ordered == 1);
    assert(reliable == 0 || reliable == 1);
    assert(provider->stream_count < H2_PEER_LOCAL_STREAM_COUNT * 2u);
    provider->stream_ids[provider->stream_count++] = stream_id;
    provider->channel_open++;
    test_channel_open_reentry_t reentry = provider->channel_open_reentry;
    provider->channel_open_reentry = TEST_CHANNEL_OPEN_REENTRY_NONE;
    if (reentry == TEST_CHANNEL_OPEN_REENTRY_TERMINAL) {
        h2_peer_webrtc_on_sctp_closed(provider->reentry_peer);
    } else if (reentry == TEST_CHANNEL_OPEN_REENTRY_PEER_CLOSE) {
        h2_pal_webrtc_peer_close(provider->reentry_api, provider->reentry_peer);
    } else if (reentry == TEST_CHANNEL_OPEN_REENTRY_OWNER_DESTROY) {
        h2_peer_destroy(provider->reentry_owner);
    } else if (reentry == TEST_CHANNEL_OPEN_REENTRY_REPLACE) {
        static const char replacement_label[] = "replacement";
        h2_peer_webrtc_on_sctp_closed(provider->reentry_peer);
        h2_pal_webrtc_channel_config_t replacement_config = {
            .label = {.data = replacement_label,
                      .len = sizeof(replacement_label) - 1u},
            .stream_id = stream_id,
            .has_stream_id = 1,
            .ordered = 1,
            .reliable = 1,
        };
        provider->replacement_result = h2_pal_webrtc_peer_create_data_channel(
            provider->reentry_api, provider->reentry_peer, &replacement_config,
            &provider->replacement_channel);
    }
    return provider->channel_open_result;
}

static h2_pal_result_t test_sctp_send(void *user, void *session,
                                      uint16_t stream_id, const uint8_t *data,
                                      size_t len, int is_text) {
    test_provider_t *provider = (test_provider_t *)user;
    assert(user == session);
    assert(stream_id <= 65534u);
    assert(data != NULL || len == 0u);
    assert(is_text == 0 || is_text == 1);
    assert(len <= sizeof(provider->last_send));
    provider->channel_send++;
    provider->last_send_stream_id = stream_id;
    memcpy(provider->last_send, data, len);
    provider->last_send_len = len;
    provider->last_send_is_text = is_text;
    return H2_PAL_OK;
}

static h2_pal_result_t test_sctp_channel_close(void *user, void *session,
                                               uint16_t stream_id) {
    test_provider_t *provider = (test_provider_t *)user;
    assert(session == provider);
    assert(stream_id <= 65534u);
    provider->channel_close++;
    assert(provider->reset_stream_count < H2_PEER_LOCAL_STREAM_COUNT);
    provider->reset_stream_ids[provider->reset_stream_count++] = stream_id;
    return provider->channel_close_result;
}

static void test_sctp_close(void *user, void *session) {
    test_provider_t *provider = (test_provider_t *)user;
    assert(session == provider);
    provider->sctp_close++;
}

static void test_peer_state(void *user, h2_pal_webrtc_peer_t *peer,
                            h2_pal_webrtc_peer_state_t state) {
    test_callbacks_t *callbacks = (test_callbacks_t *)user;
    assert(peer != NULL);
    if (state == H2_PAL_WEBRTC_PEER_CONNECTING) {
        callbacks->connecting++;
        if (callbacks->close_on_connecting) {
            h2_pal_webrtc_peer_close(callbacks->api, peer);
        }
        if (callbacks->destroy_on_connecting) {
            h2_peer_destroy(callbacks->owner);
        }
    } else if (state == H2_PAL_WEBRTC_PEER_CONNECTED) {
        callbacks->connected++;
    } else if (state == H2_PAL_WEBRTC_PEER_DISCONNECTED) {
        callbacks->disconnected++;
    } else if (state == H2_PAL_WEBRTC_PEER_FAILED) {
        callbacks->failed++;
    } else if (state == H2_PAL_WEBRTC_PEER_CLOSED) {
        callbacks->closed++;
    }
}

static void test_local_sdp(void *user, h2_pal_webrtc_peer_t *peer,
                           h2_pal_webrtc_sdp_type_t type,
                           h2_pal_webrtc_str_t sdp) {
    test_callbacks_t *callbacks = (test_callbacks_t *)user;
    assert(peer != NULL && type == H2_PAL_WEBRTC_SDP_OFFER);
    assert(sdp.len < sizeof(callbacks->local_sdp));
    memcpy(callbacks->local_sdp, sdp.data, sdp.len);
    callbacks->local_sdp_len = sdp.len;
}

static void test_channel_state(void *user, h2_pal_webrtc_peer_t *peer,
                               h2_pal_webrtc_channel_t *channel,
                               const h2_pal_webrtc_channel_info_t *info,
                               h2_pal_webrtc_channel_state_t state) {
    test_callbacks_t *callbacks = (test_callbacks_t *)user;
    assert(peer != NULL && channel != NULL && info != NULL);
    callbacks->last_channel = channel;
    if (state == H2_PAL_WEBRTC_CHANNEL_OPEN) {
        callbacks->channel_open++;
    } else if (state == H2_PAL_WEBRTC_CHANNEL_CLOSED) {
        callbacks->channel_closed++;
    } else if (state == H2_PAL_WEBRTC_CHANNEL_ERROR) {
        callbacks->channel_error++;
    }
}

static void test_opus_frame(void *user, h2_pal_webrtc_peer_t *peer,
                            const uint8_t *opus, size_t opus_len) {
    test_callbacks_t *callbacks = (test_callbacks_t *)user;
    assert(peer != NULL && opus != NULL && opus_len != 0u);
    callbacks->opus_frames++;
}

static const h2_pal_mem_vtable_t test_mem_vtable = {
    .alloc = test_alloc,
    .realloc = test_realloc,
    .free = test_free,
};

static const h2_pal_crypto_vtable_t test_crypto_vtable = {
    .random = test_random,
};

static const h2_peer_ice_provider_vtable_t test_ice_vtable = {
    .open = test_ice_open,
    .poll = test_ice_poll,
    .close = test_ice_close,
};

static int test_log_write(void *user, h2_pal_log_level_t level,
                          const char *scope, const char *message) {
  (void)user;
  (void)level;
  assert(scope != NULL);
  assert(message != NULL);
  return H2_PAL_OK;
}

static const h2_pal_log_vtable_t test_log_vtable = {
    .write = test_log_write,
};
static const h2_pal_net_vtable_t test_net_vtable = {0};
static const h2_pal_time_vtable_t test_time_vtable = {
    .get_monotonic_ms = test_monotonic_ms,
    .get_monotonic_us = test_monotonic_us,
};

static const h2_peer_dtls_provider_vtable_t test_dtls_vtable = {
    .open = test_dtls_open,
    .get_local_fingerprint = test_dtls_fingerprint,
    .set_remote_fingerprint = test_dtls_set_remote_fingerprint,
    .poll = test_dtls_poll,
    .close = test_dtls_close,
};

static const h2_peer_srtp_provider_vtable_t test_srtp_vtable = {
    .open = test_srtp_open,
    .send_rtp = test_srtp_send,
    .receive_rtp = test_srtp_receive,
    .close = test_srtp_close,
};

static const h2_peer_sctp_provider_vtable_t test_sctp_vtable = {
    .open = test_sctp_open,
    .poll = test_sctp_poll,
    .channel_open = test_sctp_channel_open,
    .send = test_sctp_send,
    .channel_close = test_sctp_channel_close,
    .close = test_sctp_close,
};

static h2_peer_config_t test_config(test_mem_t *mem) {
    static const h2_pal_log_api_t log_api = {.vtable = &test_log_vtable};
    static const h2_pal_net_api_t net_api = {.vtable = &test_net_vtable};
    static h2_pal_time_api_t time_api;
    static h2_pal_mem_api_t mem_api;
    static h2_pal_crypto_api_t crypto_api;
    mem_api.user = mem;
    mem_api.vtable = &test_mem_vtable;
    time_api.user = mem;
    time_api.vtable = &test_time_vtable;
    crypto_api.user = NULL;
    crypto_api.vtable = &test_crypto_vtable;
    h2_peer_config_t config = {
        .mem = &mem_api,
        .log = &log_api,
        .net = &net_api,
        .queue = h2_pal_unsupported_queue_api(),
        .sync = h2_pal_unsupported_sync_api(),
        .task = h2_pal_unsupported_task_api(),
        .time = &time_api,
        .crypto = &crypto_api,
        .dtls = h2_pal_unsupported_dtls_api(),
        .sctp = h2_pal_unsupported_sctp_api(),
    };
    return config;
}

static h2_peer_provider_bundle_t test_providers(test_provider_t *provider,
                                                test_mem_t *clock) {
    provider->clock = clock;
    h2_peer_provider_bundle_t providers = {
        .ice = {.user = provider, .vtable = &test_ice_vtable},
        .dtls = {.user = provider, .vtable = &test_dtls_vtable},
        .srtp = {.user = provider, .vtable = &test_srtp_vtable},
        .sctp = {.user = provider, .vtable = &test_sctp_vtable},
    };
    return providers;
}

static const char answer_sdp[] =
    "v=0\r\n"
    "a=ice-ufrag:remote\r\n"
    "a=ice-pwd:remote-password\r\n"
    "a=fingerprint:sha-256 00\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n";

static void
test_queue_stream_reset(test_provider_t *provider, uint16_t stream_id,
                        h2_pal_sctp_stream_reset_direction_t direction,
                        h2_pal_result_t result) {
    assert(provider->reset_event_count < 16u);
    provider->reset_events[provider->reset_event_count++] =
        (h2_peer_sctp_event_t){
            .type = H2_PEER_SCTP_EVENT_STREAM_RESET,
            .stream_id = stream_id,
            .reset_direction = direction,
            .reset_result = result,
        };
}

static h2_pal_webrtc_channel_config_t test_channel_config(const char *label) {
    return (h2_pal_webrtc_channel_config_t){
        .label = {.data = label, .len = strlen(label)},
        .ordered = 1,
        .reliable = 1,
    };
}

static void test_stream_pool_is_bounded_and_recycles_unopened_sid(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {0};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    test_callbacks_t callback_state = {0};
    h2_pal_webrtc_callbacks_t callbacks = {
        .user = &callback_state,
        .on_channel_state = test_channel_state,
    };
    h2_pal_webrtc_peer_t *peer = NULL;
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);

    h2_pal_webrtc_channel_config_t channel_config = test_channel_config("pool");
    h2_pal_webrtc_channel_t *channels[H2_PEER_LOCAL_STREAM_COUNT] = {0};
    for (size_t i = 0u; i < H2_PEER_LOCAL_STREAM_COUNT; ++i) {
        assert(h2_pal_webrtc_peer_create_data_channel(
                   api, peer, &channel_config, &channels[i]) == H2_PAL_OK);
        assert(channels[i]->info.stream_id == (uint16_t)(i * 2u));
    }
    h2_pal_webrtc_channel_t *extra = NULL;
    assert(h2_pal_webrtc_peer_create_data_channel(
               api, peer, &channel_config, &extra) == H2_PAL_ERR_NO_SPACE);
    assert(extra == NULL);

    h2_pal_webrtc_channel_close(api, channels[0]);
    channels[0] = NULL;
    assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &channel_config,
                                                  &extra) == H2_PAL_OK);
    assert(extra->info.stream_id == 0u);
    h2_pal_webrtc_channel_close(api, extra);
    for (size_t i = 1u; i < H2_PEER_LOCAL_STREAM_COUNT; ++i) {
        h2_pal_webrtc_channel_close(api, channels[i]);
    }
    assert(callback_state.channel_closed == H2_PEER_LOCAL_STREAM_COUNT + 1u);
    h2_pal_webrtc_peer_close(api, peer);
    h2_peer_destroy(&owner);
    assert(owner == NULL && mem.allocations == mem.frees);
}

static void test_reentrant_terminal_during_channel_open_preserves_result(void) {
    static const h2_pal_result_t provider_results[] = {
        H2_PAL_ERR_IO,
        H2_PAL_OK,
    };
    static const h2_pal_result_t expected_results[] = {
        H2_PAL_ERR_IO,
        H2_PAL_ERR_CLOSED,
    };
    for (size_t i = 0u; i < 2u; ++i) {
        test_mem_t mem = {0};
        test_provider_t provider = {
            .channel_open_result = provider_results[i],
        };
        h2_peer_config_t config = test_config(&mem);
        h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
        h2_peer_t *owner = NULL;
        assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
               H2_PAL_OK);
        const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
        test_callbacks_t callback_state = {0};
        h2_pal_webrtc_callbacks_t callbacks = {
            .user = &callback_state,
            .on_channel_state = test_channel_state,
        };
        h2_pal_webrtc_peer_t *peer = NULL;
        assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
        assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);

        provider.channel_open_reentry = TEST_CHANNEL_OPEN_REENTRY_TERMINAL;
        provider.reentry_api = api;
        provider.reentry_peer = peer;
        h2_pal_webrtc_channel_config_t channel_config =
            test_channel_config("terminal-open");
        h2_pal_webrtc_channel_t *channel =
            (h2_pal_webrtc_channel_t *)(uintptr_t)1u;
        assert(h2_pal_webrtc_peer_create_data_channel(
                   api, peer, &channel_config, &channel) ==
               expected_results[i]);
        assert(channel == NULL);
        assert(provider.channel_open == 1u);
        assert(callback_state.channel_closed == 1u);

        h2_pal_webrtc_peer_close(api, peer);
        h2_peer_destroy(&owner);
        assert(owner == NULL && mem.allocations == mem.frees);
    }
}

static void test_reentrant_terminal_while_opening_pending_channel(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {0};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    test_callbacks_t callback_state = {0};
    h2_pal_webrtc_callbacks_t callbacks = {
        .user = &callback_state,
        .on_channel_state = test_channel_state,
    };
    h2_pal_webrtc_peer_t *peer = NULL;
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);

    h2_pal_webrtc_channel_config_t channel_config =
        test_channel_config("pending-open");
    h2_pal_webrtc_channel_t *channel = NULL;
    assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &channel_config,
                                                  &channel) == H2_PAL_OK);
    assert(channel != NULL && !channel->wire_opened);

    provider.channel_open_reentry = TEST_CHANNEL_OPEN_REENTRY_TERMINAL;
    provider.reentry_api = api;
    provider.reentry_peer = peer;
    assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_ERR_CLOSED);
    assert(provider.channel_open == 1u);
    assert(callback_state.channel_closed == 1u);
    assert(peer->channels == NULL);

    h2_pal_webrtc_peer_close(api, peer);
    h2_peer_destroy(&owner);
    assert(owner == NULL && mem.allocations == mem.frees);
}

static void test_reentrant_close_during_channel_open_is_deferred(void) {
    static const struct {
        test_channel_open_reentry_t reentry;
        h2_pal_result_t provider_result;
        h2_pal_result_t expected_result;
        size_t expected_channel_closed;
    } cases[] = {
        {TEST_CHANNEL_OPEN_REENTRY_PEER_CLOSE, H2_PAL_OK, H2_PAL_ERR_CLOSED,
         1u},
        {TEST_CHANNEL_OPEN_REENTRY_OWNER_DESTROY, H2_PAL_OK, H2_PAL_ERR_CLOSED,
         1u},
        {TEST_CHANNEL_OPEN_REENTRY_PEER_CLOSE, H2_PAL_ERR_IO, H2_PAL_ERR_IO,
         0u},
        {TEST_CHANNEL_OPEN_REENTRY_OWNER_DESTROY, H2_PAL_ERR_IO, H2_PAL_ERR_IO,
         0u},
    };
    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        test_mem_t mem = {0};
        test_provider_t provider = {
            .channel_open_result = cases[i].provider_result,
        };
        h2_peer_config_t config = test_config(&mem);
        h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
        h2_peer_t *owner = NULL;
        assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
               H2_PAL_OK);
        const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
        test_callbacks_t callback_state = {0};
        h2_pal_webrtc_callbacks_t callbacks = {
            .user = &callback_state,
            .on_peer_state = test_peer_state,
            .on_channel_state = test_channel_state,
        };
        h2_pal_webrtc_peer_t *peer = NULL;
        assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
        assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);

        provider.channel_open_reentry = cases[i].reentry;
        provider.reentry_api = api;
        provider.reentry_peer = peer;
        provider.reentry_owner = &owner;
        h2_pal_webrtc_channel_config_t channel_config =
            test_channel_config("close-open");
        h2_pal_webrtc_channel_t *channel =
            (h2_pal_webrtc_channel_t *)(uintptr_t)1u;
        assert(h2_pal_webrtc_peer_create_data_channel(
                   api, peer, &channel_config, &channel) ==
               cases[i].expected_result);
        assert(channel == NULL);
        assert(provider.channel_open == 1u);
        assert(callback_state.channel_closed ==
               cases[i].expected_channel_closed);
        assert(callback_state.closed == 1u);

        h2_peer_destroy(&owner);
        assert(owner == NULL && mem.allocations == mem.frees);
    }
}

static void test_reentrant_replacement_keeps_new_generation(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {0};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    test_callbacks_t callback_state = {0};
    h2_pal_webrtc_callbacks_t callbacks = {
        .user = &callback_state,
        .on_channel_state = test_channel_state,
    };
    h2_pal_webrtc_peer_t *peer = NULL;
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);

    provider.channel_open_reentry = TEST_CHANNEL_OPEN_REENTRY_REPLACE;
    provider.reentry_api = api;
    provider.reentry_peer = peer;
    h2_pal_webrtc_channel_config_t channel_config =
        test_channel_config("original");
    h2_pal_webrtc_channel_t *channel = (h2_pal_webrtc_channel_t *)(uintptr_t)1u;
    assert(h2_pal_webrtc_peer_create_data_channel(
               api, peer, &channel_config, &channel) == H2_PAL_ERR_CLOSED);
    assert(channel == NULL);
    assert(provider.replacement_result == H2_PAL_OK);
    assert(provider.replacement_channel != NULL);
    assert(peer->channels == provider.replacement_channel);
    assert(provider.replacement_channel->info.stream_id == 0u);
    assert(provider.replacement_channel->generation == 2u);
    assert(provider.replacement_channel->wire_opened);
    assert(provider.channel_open == 2u);
    assert(callback_state.channel_closed == 1u);

    h2_pal_webrtc_peer_close(api, peer);
    assert(callback_state.channel_closed == 2u);
    h2_peer_destroy(&owner);
    assert(owner == NULL && mem.allocations == mem.frees);
}

static void test_stream_reset_quarantine_and_ordering(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {0};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    test_callbacks_t callback_state = {0};
    h2_pal_webrtc_callbacks_t callbacks = {
        .user = &callback_state,
        .on_peer_state = test_peer_state,
        .on_local_sdp = test_local_sdp,
        .on_channel_state = test_channel_state,
    };
    h2_pal_webrtc_peer_t *peer = NULL;
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
    h2_pal_webrtc_channel_config_t channel_config =
        test_channel_config("reset");
    h2_pal_webrtc_channel_t *channels[3] = {0};
    for (size_t i = 0u; i < 3u; ++i) {
        assert(h2_pal_webrtc_peer_create_data_channel(
                   api, peer, &channel_config, &channels[i]) == H2_PAL_OK);
    }
    assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);
    h2_pal_webrtc_str_t answer = {
        .data = answer_sdp,
        .len = sizeof(answer_sdp) - 1u,
    };
    assert(h2_pal_webrtc_peer_set_remote_sdp(
               api, peer, H2_PAL_WEBRTC_SDP_ANSWER, answer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);

    h2_pal_webrtc_channel_close(api, channels[0]);
    h2_pal_webrtc_channel_close(api, channels[1]);
    assert(provider.reset_stream_count == 1u);
    assert(provider.reset_stream_ids[0] == 0u);

    h2_pal_webrtc_channel_config_t independent_config = channel_config;
    independent_config.has_stream_id = 1;
    independent_config.stream_id = 6u;
    h2_pal_webrtc_channel_t *independent = NULL;
    assert(h2_pal_webrtc_peer_create_data_channel(
               api, peer, &independent_config, &independent) == H2_PAL_OK);
    assert(independent != NULL && provider.channel_open == 4u);

    h2_pal_webrtc_channel_config_t explicit_config = channel_config;
    explicit_config.has_stream_id = 1;
    explicit_config.stream_id = 0u;
    h2_pal_webrtc_channel_t *replacement0 = NULL;
    assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &explicit_config,
                                                  &replacement0) ==
           H2_PAL_ERR_INVALID_ARG);
    test_queue_stream_reset(&provider, 0u,
                            H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET, H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &explicit_config,
                                                  &replacement0) ==
           H2_PAL_ERR_INVALID_ARG);

    test_queue_stream_reset(
        &provider, 0u, H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED, H2_PAL_OK);
    test_queue_stream_reset(
        &provider, 0u, H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED, H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(provider.reset_stream_count == 2u);
    assert(provider.reset_stream_ids[1] == 2u);
    assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &explicit_config,
                                                  &replacement0) == H2_PAL_OK);

    explicit_config.stream_id = 2u;
    h2_pal_webrtc_channel_t *replacement2 = NULL;
    test_queue_stream_reset(
        &provider, 2u, H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED, H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &explicit_config,
                                                  &replacement2) ==
           H2_PAL_ERR_INVALID_ARG);
    test_queue_stream_reset(&provider, 2u,
                            H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET, H2_PAL_OK);
    test_queue_stream_reset(&provider, 2u,
                            H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET, H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &explicit_config,
                                                  &replacement2) == H2_PAL_OK);

    test_queue_stream_reset(&provider, 4u,
                            H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET, H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(provider.reset_stream_count == 3u);
    assert(provider.reset_stream_ids[2] == 4u);
    explicit_config.stream_id = 4u;
    h2_pal_webrtc_channel_t *replacement4 = NULL;
    assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &explicit_config,
                                                  &replacement4) ==
           H2_PAL_ERR_INVALID_ARG);
    test_queue_stream_reset(
        &provider, 4u, H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED, H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &explicit_config,
                                                  &replacement4) == H2_PAL_OK);

    h2_pal_webrtc_peer_close(api, peer);
    h2_peer_destroy(&owner);
    assert(owner == NULL && mem.allocations == mem.frees);
}

static void test_wire_visible_full_pool_turnover(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {0};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    test_callbacks_t callback_state = {0};
    h2_pal_webrtc_callbacks_t callbacks = {
        .user = &callback_state,
        .on_peer_state = test_peer_state,
        .on_local_sdp = test_local_sdp,
        .on_channel_state = test_channel_state,
    };
    h2_pal_webrtc_peer_t *peer = NULL;
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
    h2_pal_webrtc_channel_config_t channel_config =
        test_channel_config("turnover");
    h2_pal_webrtc_channel_t *channels[H2_PEER_LOCAL_STREAM_COUNT] = {0};
    for (size_t i = 0u; i < H2_PEER_LOCAL_STREAM_COUNT; ++i) {
        assert(h2_pal_webrtc_peer_create_data_channel(
                   api, peer, &channel_config, &channels[i]) == H2_PAL_OK);
    }
    assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);
    h2_pal_webrtc_str_t answer = {
        .data = answer_sdp,
        .len = sizeof(answer_sdp) - 1u,
    };
    assert(h2_pal_webrtc_peer_set_remote_sdp(
               api, peer, H2_PAL_WEBRTC_SDP_ANSWER, answer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(callback_state.channel_open == H2_PEER_LOCAL_STREAM_COUNT);

    for (size_t i = 0u; i < H2_PEER_LOCAL_STREAM_COUNT; ++i) {
        h2_pal_webrtc_channel_close(api, channels[i]);
    }
    assert(provider.reset_stream_count == 1u);
    for (size_t i = 0u; i < H2_PEER_LOCAL_STREAM_COUNT; ++i) {
        uint16_t stream_id = (uint16_t)(i * 2u);
        h2_pal_sctp_stream_reset_direction_t first =
            (i & 1u) == 0u ? H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED
                           : H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET;
        h2_pal_sctp_stream_reset_direction_t second =
            first == H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED
                ? H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET
                : H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED;
        test_queue_stream_reset(&provider, stream_id, first, H2_PAL_OK);
        test_queue_stream_reset(&provider, stream_id, second, H2_PAL_OK);
        assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
        size_t expected_submissions = i + 2u;
        if (expected_submissions > H2_PEER_LOCAL_STREAM_COUNT) {
            expected_submissions = H2_PEER_LOCAL_STREAM_COUNT;
        }
        assert(provider.reset_stream_count == expected_submissions);
    }
    assert(callback_state.channel_closed == H2_PEER_LOCAL_STREAM_COUNT);
    assert(mem.allocations - mem.frees == 2u);

    memset(channels, 0, sizeof(channels));
    for (size_t i = 0u; i < H2_PEER_LOCAL_STREAM_COUNT; ++i) {
        assert(h2_pal_webrtc_peer_create_data_channel(
                   api, peer, &channel_config, &channels[i]) == H2_PAL_OK);
        assert(channels[i]->info.stream_id == (uint16_t)(i * 2u));
    }
    assert(provider.channel_open == H2_PEER_LOCAL_STREAM_COUNT * 2u);
    assert(mem.allocations - mem.frees == 2u + H2_PEER_LOCAL_STREAM_COUNT * 2u);
    h2_pal_webrtc_peer_close(api, peer);
    h2_peer_destroy(&owner);
    assert(owner == NULL && mem.allocations == mem.frees);
}

static void test_stream_reset_backpressure_and_fatal_failure(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {0};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    test_callbacks_t callback_state = {0};
    h2_pal_webrtc_callbacks_t callbacks = {
        .user = &callback_state,
        .on_peer_state = test_peer_state,
        .on_local_sdp = test_local_sdp,
        .on_channel_state = test_channel_state,
    };
    h2_pal_webrtc_peer_t *peer = NULL;
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
    h2_pal_webrtc_channel_config_t channel_config =
        test_channel_config("failure");
    h2_pal_webrtc_channel_t *channels[2] = {0};
    assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &channel_config,
                                                  &channels[0]) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &channel_config,
                                                  &channels[1]) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);
    h2_pal_webrtc_str_t answer = {
        .data = answer_sdp,
        .len = sizeof(answer_sdp) - 1u,
    };
    assert(h2_pal_webrtc_peer_set_remote_sdp(
               api, peer, H2_PAL_WEBRTC_SDP_ANSWER, answer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);

    provider.channel_close_result = H2_PAL_ERR_BUSY;
    h2_pal_webrtc_channel_close(api, channels[0]);
    assert(provider.reset_stream_count == 1u);
    provider.channel_close_result = H2_PAL_OK;
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(provider.reset_stream_count == 2u);
    assert(provider.reset_stream_ids[0] == 0u &&
           provider.reset_stream_ids[1] == 0u);
    test_queue_stream_reset(
        &provider, 0u, H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED, H2_PAL_OK);
    test_queue_stream_reset(&provider, 0u,
                            H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET, H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);

    h2_pal_webrtc_channel_config_t explicit_config = channel_config;
    explicit_config.has_stream_id = 1;
    explicit_config.stream_id = 0u;
    h2_pal_webrtc_channel_t *replacement = NULL;
    assert(h2_pal_webrtc_peer_create_data_channel(api, peer, &explicit_config,
                                                  &replacement) == H2_PAL_OK);
    provider.channel_close_result = H2_PAL_ERR_IO;
    h2_pal_webrtc_channel_close(api, channels[1]);
    assert(callback_state.failed == 1u);
    assert(callback_state.channel_error == 1u);
    h2_pal_webrtc_peer_close(api, peer);
    h2_peer_destroy(&owner);
    assert(owner == NULL && mem.allocations == mem.frees);
}

static void test_remote_channel_lifecycle(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {0};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    test_callbacks_t callback_state = {0};
    h2_pal_webrtc_callbacks_t callbacks = {
        .user = &callback_state,
        .on_peer_state = test_peer_state,
        .on_local_sdp = test_local_sdp,
        .on_channel_state = test_channel_state,
    };
    h2_pal_webrtc_peer_t *peer = NULL;
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
    peer->local_stream_first = 1u;
    peer->next_stream_id = 1u;
    assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);
    const h2_pal_webrtc_str_t answer = {
        .data = answer_sdp,
        .len = sizeof(answer_sdp) - 1u,
    };
    assert(h2_pal_webrtc_peer_set_remote_sdp(
               api, peer, H2_PAL_WEBRTC_SDP_ANSWER, answer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);

    const char reverse_label[] = "server/reverse/0";
    const h2_pal_webrtc_str_t reverse = {
        .data = reverse_label,
        .len = sizeof(reverse_label) - 1u,
    };
    assert(h2_peer_webrtc_on_remote_channel_open(peer, reverse, 2u, 1, 1) ==
           H2_PAL_OK);
    h2_pal_webrtc_channel_t *remote = callback_state.last_channel;
    assert(remote != NULL && remote->remote_created && remote->open);
    assert(remote->info.stream_id == 2u);
    assert(callback_state.channel_open == 1u);
    assert(h2_peer_webrtc_on_remote_channel_open(peer, reverse, 2u, 1, 1) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_peer_webrtc_on_remote_channel_open(peer, reverse, 3u, 1, 1) ==
           H2_PAL_ERR_INVALID_ARG);

    const uint8_t reply[] = "reverse-reply";
    assert(h2_pal_webrtc_channel_send(api, remote, reply, sizeof(reply) - 1u,
                                      1) == H2_PAL_OK);
    assert(provider.channel_send == 1u);
    assert(provider.last_send_stream_id == 2u);
    assert(provider.last_send_is_text == 1);
    assert(provider.last_send_len == sizeof(reply) - 1u);
    assert(memcmp(provider.last_send, reply, sizeof(reply) - 1u) == 0);

    test_queue_stream_reset(&provider, 2u,
                            H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET, H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(callback_state.channel_closed == 1u);
    assert(provider.reset_stream_count == 1u);
    assert(provider.reset_stream_ids[0] == 2u);
    test_queue_stream_reset(
        &provider, 2u, H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED, H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(!peer->stream_resets[2u].active);

    test_queue_stream_reset(&provider, 2u,
                            H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET, H2_PAL_OK);
    test_queue_stream_reset(
        &provider, 2u, H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED, H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(callback_state.channel_closed == 1u);
    assert(provider.reset_stream_count == 1u);

    assert(h2_peer_webrtc_on_remote_channel_open(peer, reverse, 4u, 0, 0) ==
           H2_PAL_OK);
    remote = callback_state.last_channel;
    assert(remote != NULL && !remote->info.ordered && !remote->info.reliable);
    h2_pal_webrtc_channel_close(api, remote);
    assert(callback_state.channel_closed == 2u);
    assert(provider.reset_stream_count == 2u);
    assert(provider.reset_stream_ids[1] == 4u);
    test_queue_stream_reset(
        &provider, 4u, H2_PAL_SCTP_STREAM_RESET_OUTGOING_COMPLETED, H2_PAL_OK);
    test_queue_stream_reset(&provider, 4u,
                            H2_PAL_SCTP_STREAM_RESET_INCOMING_RESET, H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(!peer->stream_resets[4u].active);
    assert(callback_state.channel_closed == 2u);

    h2_pal_webrtc_peer_close(api, peer);
    h2_peer_destroy(&owner);
    assert(owner == NULL && mem.allocations == mem.frees);
}

static void test_complete_lifecycle(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {.block_next_srtp_send = 1};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    assert(api != NULL);

    test_callbacks_t callback_state = {0};
    h2_pal_webrtc_callbacks_t callbacks = {
        .user = &callback_state,
        .on_peer_state = test_peer_state,
        .on_local_sdp = test_local_sdp,
        .on_channel_state = test_channel_state,
        .on_opus_frame = test_opus_frame,
    };
    h2_pal_webrtc_peer_t *peer = NULL;
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);

    char ice_url[] = "stun:example.invalid:3478";
    h2_pal_webrtc_ice_server_t ice_server = {
        .url = {.data = ice_url, .len = sizeof(ice_url) - 1u},
    };
    assert(h2_pal_webrtc_peer_add_ice_server(api, peer, &ice_server) ==
           H2_PAL_OK);
    memset(ice_url, 'x', sizeof(ice_url) - 1u);
    assert(strcmp(peer->ice_servers[0].url, "stun:example.invalid:3478") == 0);

    const char *labels[] = {"alpha", "beta", "gamma"};
    h2_pal_webrtc_channel_t *channels[3] = {0};
    for (size_t i = 0u; i < 3u; ++i) {
        h2_pal_webrtc_channel_config_t channel_config = {
            .label = {.data = labels[i], .len = strlen(labels[i])},
            .ordered = i != 2u,
            .reliable = i != 1u,
        };
        assert(h2_pal_webrtc_peer_create_data_channel(
                   api, peer, &channel_config, &channels[i]) == H2_PAL_OK);
    }

    assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);
    assert(callback_state.connecting == 1u &&
           callback_state.local_sdp_len != 0u);
    assert(provider.channel_open == 3u);
    h2_pal_webrtc_str_t answer = {.data = answer_sdp,
                                  .len = sizeof(answer_sdp) - 1u};
    assert(h2_pal_webrtc_peer_set_remote_sdp(
               api, peer, H2_PAL_WEBRTC_SDP_ANSWER, answer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(callback_state.connected == 1u && callback_state.channel_open == 3u);
    assert(provider.ice_timeout_ms == 10);
    assert(provider.dtls_timeout_ms == 6);
    assert(provider.sctp_timeout_ms == 2);

    const uint8_t opus[] = {0xf8u, 0x42u};
    assert(h2_pal_webrtc_peer_send_opus(api, peer, opus, sizeof(opus)) ==
           H2_PAL_ERR_WOULD_BLOCK);
    assert(h2_pal_webrtc_peer_send_opus(api, peer, opus, sizeof(opus)) ==
           H2_PAL_OK);
    assert(provider.last_rtp_len == 14u);
    assert(provider.last_rtp[0] == 0x80u && provider.last_rtp[1] == 0x6fu);
    assert(provider.last_rtp[2] == 0u && provider.last_rtp[3] == 0u);
    assert(h2_peer_receive_rtp_for_test(peer, provider.last_rtp,
                                        provider.last_rtp_len) == H2_PAL_OK);
    assert(callback_state.opus_frames == 1u);

    const uint8_t message[] = "hello";
    assert(h2_pal_webrtc_channel_send(api, channels[0], message,
                                      sizeof(message) - 1u, 1) == H2_PAL_OK);
    h2_pal_webrtc_channel_close(api, channels[0]);
    assert(provider.channel_close == 1u);
    provider.dtls_poll_result = H2_PAL_ERR_TIMEOUT;
    assert(h2_pal_webrtc_peer_poll(api, peer, 1) == H2_PAL_ERR_TIMEOUT);
    assert(provider.dtls_close == 0u && callback_state.connected == 1u);
    provider.dtls_poll_result = H2_PAL_ERR_CLOSED;
    assert(h2_pal_webrtc_peer_poll(api, peer, 1) == H2_PAL_ERR_CLOSED);
    assert(callback_state.disconnected == 1u);
    h2_pal_webrtc_peer_close(api, peer);
    assert(provider.ice_close == 1u && provider.dtls_close == 1u &&
           provider.srtp_close == 1u && provider.sctp_close == 1u);
    assert(callback_state.closed == 1u && callback_state.channel_closed == 3u);
    h2_peer_destroy(&owner);
    assert(owner == NULL && mem.allocations == mem.frees);
}

static void test_dtls_provider_handshake_failure_is_terminal(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {.dtls_poll_result = H2_PAL_ERR_TLS_VERIFY};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    test_callbacks_t callback_state = {0};
    h2_pal_webrtc_callbacks_t callbacks = {
        .user = &callback_state,
        .on_peer_state = test_peer_state,
        .on_local_sdp = test_local_sdp,
    };
    h2_pal_webrtc_peer_t *peer = NULL;
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);
    h2_pal_webrtc_str_t answer = {
        .data = answer_sdp,
        .len = sizeof(answer_sdp) - 1u,
    };
    assert(h2_pal_webrtc_peer_set_remote_sdp(
               api, peer, H2_PAL_WEBRTC_SDP_ANSWER, answer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_ERR_TLS_VERIFY);
    assert(callback_state.failed == 1u);
    assert(callback_state.connected == 0u);

    h2_pal_webrtc_peer_close(api, peer);
    h2_peer_destroy(&owner);
    assert(owner == NULL && mem.allocations == mem.frees);
}

static void test_partial_provider_failure(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {.fail_srtp_open = 1};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    h2_pal_webrtc_callbacks_t callbacks = {0};
    h2_pal_webrtc_peer_t *peer = NULL;
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_ERR_IO);
    assert(provider.ice_close == 1u && provider.dtls_close == 1u &&
           provider.srtp_close == 0u && provider.sctp_close == 0u);
    h2_peer_destroy(&owner);
    assert(mem.allocations == mem.frees);
}

static void test_dtls_provider_failure_closes_ice(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {.fail_dtls_open = 1};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    h2_pal_webrtc_callbacks_t callbacks = {0};
    h2_pal_webrtc_peer_t *peer = NULL;
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_ERR_IO);
    assert(provider.ice_open == 1u && provider.ice_close == 1u);
    assert(provider.dtls_open == 1u && provider.dtls_close == 0u);
    h2_peer_destroy(&owner);
    assert(mem.allocations == mem.frees);
}

static void test_connecting_callback_can_close_peer(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {0};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    test_callbacks_t callback_state = {
        .api = api,
        .close_on_connecting = 1,
    };
    h2_pal_webrtc_callbacks_t callbacks = {
        .user = &callback_state,
        .on_peer_state = test_peer_state,
        .on_local_sdp = test_local_sdp,
    };
    h2_pal_webrtc_peer_t *peer = NULL;
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_ERR_CLOSED);
    assert(callback_state.connecting == 1u && callback_state.closed == 1u);
    assert(callback_state.local_sdp_len == 0u);
    assert(provider.ice_close == 1u && provider.dtls_close == 1u &&
           provider.srtp_close == 1u && provider.sctp_close == 1u);
    h2_peer_destroy(&owner);
    assert(mem.allocations == mem.frees);
}

static void test_connecting_callback_can_destroy_owner(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {0};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    test_callbacks_t callback_state = {
        .api = api,
        .owner = &owner,
        .destroy_on_connecting = 1,
    };
    h2_pal_webrtc_callbacks_t callbacks = {
        .user = &callback_state,
        .on_peer_state = test_peer_state,
        .on_local_sdp = test_local_sdp,
    };
    h2_pal_webrtc_callbacks_t idle_callbacks = {0};
    h2_pal_webrtc_peer_t *idle_peer = NULL;
    h2_pal_webrtc_peer_t *peer = NULL;
    assert(h2_pal_webrtc_peer_create(api, &idle_callbacks, &idle_peer) ==
           H2_PAL_OK);
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_ERR_CLOSED);
    assert(owner == NULL);
    assert(callback_state.connecting == 1u && callback_state.closed == 1u);
    assert(callback_state.local_sdp_len == 0u);
    assert(provider.ice_close == 1u && provider.dtls_close == 1u &&
           provider.srtp_close == 1u && provider.sctp_close == 1u);
    assert(mem.allocations == mem.frees);
}

static void test_allocation_failure(void) {
    test_mem_t mem = {.fail_at = 2u};
    test_provider_t provider = {0};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    h2_pal_webrtc_callbacks_t callbacks = {0};
    h2_pal_webrtc_peer_t *peer = (h2_pal_webrtc_peer_t *)(uintptr_t)1u;
    assert(h2_pal_webrtc_peer_create(h2_peer_webrtc_api(owner), &callbacks,
                                     &peer) == H2_PAL_ERR_NO_MEMORY);
    assert(peer == NULL);
    h2_peer_destroy(&owner);
    assert(mem.allocations == mem.frees);
}

static void test_public_instance_rejects_missing_pal_operations(void) {
    test_mem_t mem = {0};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_t *owner = (h2_peer_t *)(uintptr_t)1u;
    assert(h2_peer_create(&config, &owner) == H2_PAL_ERR_UNSUPPORTED);
    assert(owner == NULL);

    config.net = h2_pal_unsupported_net_api();
    h2_pal_time_vtable_t missing_time_operation = *config.time->vtable;
    missing_time_operation.get_monotonic_us = NULL;
    const h2_pal_time_api_t incomplete_time = {
        .vtable = &missing_time_operation,
    };
    config.time = &incomplete_time;
    owner = (h2_peer_t *)(uintptr_t)1u;
    assert(h2_peer_create(&config, &owner) == H2_PAL_ERR_INVALID_ARG);
    assert(owner == NULL);
    assert(mem.allocations == mem.frees);
}

static void test_instance_rejects_incomplete_log(void) {
  test_mem_t mem = {0};
  test_provider_t provider = {0};
  h2_peer_config_t config = test_config(&mem);
  h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
  const h2_pal_log_vtable_t incomplete_vtable = {0};
  const h2_pal_log_api_t incomplete_log = {
      .vtable = &incomplete_vtable,
  };
  config.log = &incomplete_log;

  h2_peer_t *owner = (h2_peer_t *)(uintptr_t)1u;
  assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(owner == NULL);
  assert(mem.allocations == mem.frees);
}

static void test_public_instance_requires_bounded_tcp_send(void) {
  test_mem_t mem = {0};
  h2_peer_config_t config = test_config(&mem);
  config.net = h2_pal_unsupported_net_api();
  config.time = h2_pal_unsupported_time_api();

  h2_peer_t *owner = NULL;
  assert(h2_peer_create(&config, &owner) == H2_PAL_OK);
  h2_peer_destroy(&owner);
  assert(owner == NULL);

  h2_pal_net_vtable_t missing_operation = *config.net->vtable;
  missing_operation.tcp_send_timeout = NULL;
  const h2_pal_net_api_t incomplete_net = {
      .vtable = &missing_operation,
  };
  config.net = &incomplete_net;
  owner = (h2_peer_t *)(uintptr_t)1u;
  assert(h2_peer_create(&config, &owner) == H2_PAL_ERR_UNSUPPORTED);
  assert(owner == NULL);
  assert(mem.allocations == mem.frees);
}

static void test_ice_server_transport_validation(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {0};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    h2_pal_webrtc_callbacks_t callbacks = {0};
    h2_pal_webrtc_peer_t *peer = NULL;
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);

    const char username[] = "user";
    const char credential[] = "secret";
    h2_pal_webrtc_ice_server_t server = {
        .username = {.data = username, .len = sizeof(username) - 1u},
        .credential = {.data = credential, .len = sizeof(credential) - 1u},
    };
    const char secure_url[] = "turns:example.invalid:5349";
    server.url = (h2_pal_webrtc_str_t){
        .data = secure_url,
        .len = sizeof(secure_url) - 1u,
    };
    assert(h2_pal_webrtc_peer_add_ice_server(api, peer, &server) ==
           H2_PAL_ERR_UNSUPPORTED);

    const char tcp_url[] = "turn:example.invalid:3478?transport=tcp";
    server.url = (h2_pal_webrtc_str_t){
        .data = tcp_url,
        .len = sizeof(tcp_url) - 1u,
    };
    assert(h2_pal_webrtc_peer_add_ice_server(api, peer, &server) ==
           H2_PAL_ERR_UNSUPPORTED);

    const char udp_url[] = "turn:example.invalid:3478?transport=udp";
    server.url = (h2_pal_webrtc_str_t){
        .data = udp_url,
        .len = sizeof(udp_url) - 1u,
    };
    server.credential = (h2_pal_webrtc_str_t){0};
    assert(h2_pal_webrtc_peer_add_ice_server(api, peer, &server) ==
           H2_PAL_ERR_INVALID_ARG);
    server.credential = (h2_pal_webrtc_str_t){
        .data = credential,
        .len = sizeof(credential) - 1u,
    };
    assert(h2_pal_webrtc_peer_add_ice_server(api, peer, &server) == H2_PAL_OK);

    h2_pal_webrtc_peer_close(api, peer);
    h2_peer_destroy(&owner);
    assert(mem.allocations == mem.frees);
}

static h2_pal_result_t test_track_read(void *user, uint8_t *opus,
                                       size_t capacity, size_t *out_len) {
    (void)user;
    (void)opus;
    (void)capacity;
    *out_len = 0u;
    return H2_PAL_ERR_WOULD_BLOCK;
}

typedef struct test_media_track_state {
    int read_ready;
    size_t reads;
    size_t writes;
    uint8_t last_write[8];
    size_t last_write_len;
} test_media_track_state_t;

static h2_pal_result_t test_active_track_read(void *user, uint8_t *opus,
                                              size_t capacity,
                                              size_t *out_len) {
    test_media_track_state_t *state = user;
    if (!state->read_ready)
        return H2_PAL_ERR_WOULD_BLOCK;
    assert(capacity >= 2u);
    opus[0] = 0xf8u;
    opus[1] = 0x42u;
    *out_len = 2u;
    state->read_ready = 0;
    state->reads++;
    return H2_PAL_OK;
}

static h2_pal_result_t test_active_track_write(void *user, const uint8_t *opus,
                                               size_t len) {
    test_media_track_state_t *state = user;
    assert(len <= sizeof(state->last_write));
    memcpy(state->last_write, opus, len);
    state->last_write_len = len;
    state->writes++;
    return H2_PAL_OK;
}

static void test_media_track_poll_retains_opus_across_backpressure(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {0};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    test_media_track_state_t track_state = {.read_ready = 1};
    const h2_peer_media_track_config_t track_config = {
        .user = &track_state,
        .read = test_active_track_read,
        .write = test_active_track_write,
    };
    h2_pal_webrtc_track_t *track = NULL;
    assert(h2_peer_media_track_create(owner, &track_config, &track) ==
           H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    const h2_pal_webrtc_callbacks_t callbacks = {0};
    h2_pal_webrtc_peer_t *peer = NULL;
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_set_media_track(api, peer, track) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);
    const h2_pal_webrtc_str_t answer = {
        .data = answer_sdp,
        .len = sizeof(answer_sdp) - 1u,
    };
    assert(h2_pal_webrtc_peer_set_remote_sdp(
               api, peer, H2_PAL_WEBRTC_SDP_ANSWER, answer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(track_state.reads == 0u);
    provider.block_next_srtp_send = 1;
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(!track_state.read_ready && track_state.reads == 1u);
    assert(provider.srtp_send_calls == 1u && provider.last_rtp_len == 0u);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(track_state.reads == 1u);
    assert(provider.srtp_send_calls == 2u && provider.last_rtp_len == 14u);
    assert(h2_peer_receive_rtp_for_test(peer, provider.last_rtp,
                                        provider.last_rtp_len) == H2_PAL_OK);
    assert(track_state.writes == 1u && track_state.last_write_len == 2u);
    h2_pal_webrtc_peer_close(api, peer);
    assert(h2_peer_media_track_destroy(&track) == H2_PAL_OK);
    h2_peer_destroy(&owner);
    assert(mem.allocations == mem.frees);
}

static void test_media_track_lifecycle(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {0};
    h2_peer_config_t config = test_config(&mem);
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    const h2_peer_media_track_config_t track_config = {
        .read = test_track_read,
    };
    h2_pal_webrtc_track_t *track = NULL;
    assert(h2_peer_media_track_create(owner, &track_config, &track) ==
           H2_PAL_OK);
    assert(track != NULL);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    const h2_pal_webrtc_callbacks_t callbacks = {0};
    h2_pal_webrtc_peer_t *peer = NULL;
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_set_media_track(api, peer, track) == H2_PAL_OK);
    assert(h2_peer_media_track_destroy(&track) == H2_PAL_ERR_INVALID_STATE);
    assert(track != NULL);
    assert(h2_pal_webrtc_peer_set_media_track(api, peer, NULL) == H2_PAL_OK);
    assert(h2_peer_media_track_destroy(&track) == H2_PAL_OK);
    assert(track == NULL);
    h2_pal_webrtc_peer_close(api, peer);
    h2_peer_destroy(&owner);
    assert(mem.allocations == mem.frees);
}

/* Single-token gate standing in for the h2peer/opus/rx queue: send_latest
 * arms it, recv consumes it or reports TIMEOUT when nothing is pending. */
typedef struct test_opus_gate {
    int armed;
} test_opus_gate_t;

static int test_opus_gate_send_latest(void *user, h2_pal_queue_t *queue,
                                      const void *item) {
    (void)user;
    (void)item;
    ((test_opus_gate_t *)queue)->armed = 1;
    return H2_PAL_OK;
}

static int test_opus_gate_recv(void *user, h2_pal_queue_t *queue,
                               void *out_item, uint32_t timeout_ms) {
    (void)user;
    (void)timeout_ms;
    test_opus_gate_t *gate = (test_opus_gate_t *)queue;
    if (!gate->armed) {
        return H2_PAL_ERR_TIMEOUT;
    }
    gate->armed = 0;
    *(uint8_t *)out_item = 1u;
    return H2_PAL_OK;
}

static const h2_pal_queue_vtable_t test_opus_gate_vtable = {
    .send_latest = test_opus_gate_send_latest,
    .recv = test_opus_gate_recv,
};

typedef struct test_rtp_packet {
    uint8_t data[64];
    size_t len;
} test_rtp_packet_t;

/* Builds one Opus RTP packet through the direct send path so the pull test
 * can replay it after the peer switches to production delivery. */
static void test_build_opus_rtp(const h2_pal_webrtc_api_t *api,
                                h2_pal_webrtc_peer_t *peer,
                                test_provider_t *provider, uint8_t marker,
                                test_rtp_packet_t *out_packet) {
    const uint8_t opus[] = {0xf8u, marker};
    assert(h2_pal_webrtc_peer_send_opus(api, peer, opus, sizeof(opus)) ==
           H2_PAL_OK);
    assert(provider->last_rtp_len == 14u &&
           provider->last_rtp_len <= sizeof(out_packet->data));
    memcpy(out_packet->data, provider->last_rtp, provider->last_rtp_len);
    out_packet->len = provider->last_rtp_len;
}

/* Opus pull delivery drops a frame when the four-slot mailbox is full: the
 * transport result stays H2_PAL_OK, queued frames remain readable, and the
 * mailbox accepts new frames once the consumer drains it. */
static void test_opus_pull_mailbox_full_drops_frame_without_terminal(void) {
    test_mem_t mem = {0};
    test_provider_t provider = {0};
    h2_peer_config_t config = test_config(&mem);
    test_opus_gate_t gate = {0};
    const h2_pal_queue_api_t gate_api = {.user = NULL,
                                         .vtable = &test_opus_gate_vtable};
    config.queue = &gate_api;
    h2_peer_provider_bundle_t providers = test_providers(&provider, &mem);
    h2_peer_t *owner = NULL;
    assert(h2_peer_create_with_providers(&config, &providers, &owner) ==
           H2_PAL_OK);
    const h2_pal_webrtc_api_t *api = h2_peer_webrtc_api(owner);
    test_callbacks_t callback_state = {0};
    h2_pal_webrtc_callbacks_t callbacks = {
        .user = &callback_state,
        .on_peer_state = test_peer_state,
        .on_local_sdp = test_local_sdp,
        .on_channel_state = test_channel_state,
    };
    h2_pal_webrtc_peer_t *peer = NULL;
    assert(h2_pal_webrtc_peer_create(api, &callbacks, &peer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_start_offer(api, peer) == H2_PAL_OK);
    h2_pal_webrtc_str_t answer = {.data = answer_sdp,
                                  .len = sizeof(answer_sdp) - 1u};
    assert(h2_pal_webrtc_peer_set_remote_sdp(
               api, peer, H2_PAL_WEBRTC_SDP_ANSWER, answer) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_poll(api, peer, 10) == H2_PAL_OK);
    assert(callback_state.connected == 1u);

    test_rtp_packet_t packets[H2_PEER_OUTPUT_SLOT_COUNT + 2u];
    for (size_t i = 0u; i < H2_PEER_OUTPUT_SLOT_COUNT + 2u; ++i) {
        test_build_opus_rtp(api, peer, &provider, (uint8_t)(0x10u + i),
                            &packets[i]);
    }

    /* Switch the connected peer to production pull delivery. The network
     * event queue only has to be non-NULL; the wakeup flag is pre-armed so
     * the mailbox never posts a RECEIVE_READY event through it. */
    int sentinel_events = 0;
    owner->production_backend = 1;
    peer->receive_flags = H2_PAL_WEBRTC_RECEIVE_OPUS_PULL;
    peer->network_events = (h2_pal_queue_t *)&sentinel_events;
    peer->opus_rx_gate = (h2_pal_queue_t *)&gate;
    atomic_store(&peer->network_receive_wakeup_queued, 1);

    for (size_t i = 0u; i < H2_PEER_OUTPUT_SLOT_COUNT; ++i) {
        assert(h2_peer_receive_rtp_for_test(peer, packets[i].data,
                                            packets[i].len) == H2_PAL_OK);
    }
    assert(atomic_load(&peer->opus_rx_count) == H2_PEER_OUTPUT_SLOT_COUNT);
    assert(atomic_load(&peer->network_receive_full) == 1u);

    /* Fifth frame: mailbox full, frame dropped, peer stays healthy. */
    assert(h2_peer_receive_rtp_for_test(
               peer, packets[H2_PEER_OUTPUT_SLOT_COUNT].data,
               packets[H2_PEER_OUTPUT_SLOT_COUNT].len) == H2_PAL_OK);
    assert(atomic_load(&peer->network_transport_result) == H2_PAL_OK);
    assert(atomic_load(&peer->opus_rx_count) == H2_PEER_OUTPUT_SLOT_COUNT);
    assert(!peer->closed);

    uint8_t frame[H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE];
    size_t frame_len = 0u;
    for (uint8_t i = 0u; i < H2_PEER_OUTPUT_SLOT_COUNT; ++i) {
        assert(h2_pal_webrtc_peer_receive_opus(api, peer, frame, sizeof(frame),
                                               &frame_len, 0) == H2_PAL_OK);
        assert(frame_len == 2u && frame[0] == 0xf8u &&
               frame[1] == (uint8_t)(0x10u + i));
    }
    assert(h2_pal_webrtc_peer_receive_opus(api, peer, frame, sizeof(frame),
                                           &frame_len, 0) == H2_PAL_ERR_TIMEOUT);
    assert(atomic_load(&peer->opus_rx_count) == 0u);
    assert(atomic_load(&peer->network_receive_full) == 0u);

    /* The mailbox keeps working after the overflow. */
    assert(h2_peer_receive_rtp_for_test(
               peer, packets[H2_PEER_OUTPUT_SLOT_COUNT + 1u].data,
               packets[H2_PEER_OUTPUT_SLOT_COUNT + 1u].len) == H2_PAL_OK);
    assert(h2_pal_webrtc_peer_receive_opus(api, peer, frame, sizeof(frame),
                                           &frame_len, 0) == H2_PAL_OK);
    assert(frame_len == 2u &&
           frame[1] == (uint8_t)(0x10u + H2_PEER_OUTPUT_SLOT_COUNT + 1u));
    assert(atomic_load(&peer->network_transport_result) == H2_PAL_OK);

    /* Hand the peer back to the direct backend for teardown. */
    for (size_t i = 0u; i < H2_PEER_OUTPUT_SLOT_COUNT; ++i) {
        h2_peer_tx_item_t *item = peer->opus_rx_storage[i];
        if (item != NULL) {
            h2_pal_mem_free(owner->config.mem, item->data);
            h2_pal_mem_free(owner->config.mem, item);
            peer->opus_rx_storage[i] = NULL;
        }
    }
    peer->opus_rx_gate = NULL;
    peer->network_events = NULL;
    peer->receive_flags = 0u;
    owner->production_backend = 0;
    h2_pal_webrtc_peer_close(api, peer);
    h2_peer_destroy(&owner);
    assert(owner == NULL && mem.allocations == mem.frees);
}

int main(void) {
    test_stream_pool_is_bounded_and_recycles_unopened_sid();
    test_reentrant_terminal_during_channel_open_preserves_result();
    test_reentrant_terminal_while_opening_pending_channel();
    test_reentrant_close_during_channel_open_is_deferred();
    test_reentrant_replacement_keeps_new_generation();
    test_stream_reset_quarantine_and_ordering();
    test_wire_visible_full_pool_turnover();
    test_stream_reset_backpressure_and_fatal_failure();
    test_remote_channel_lifecycle();
    test_complete_lifecycle();
    test_dtls_provider_handshake_failure_is_terminal();
    test_partial_provider_failure();
    test_dtls_provider_failure_closes_ice();
    test_connecting_callback_can_close_peer();
    test_connecting_callback_can_destroy_owner();
    test_allocation_failure();
    test_instance_rejects_incomplete_log();
    test_public_instance_rejects_missing_pal_operations();
    test_public_instance_requires_bounded_tcp_send();
    test_ice_server_transport_validation();
    test_media_track_lifecycle();
    test_media_track_poll_retains_opus_across_backpressure();
    test_opus_pull_mailbox_full_drops_frame_without_terminal();
    return 0;
}
