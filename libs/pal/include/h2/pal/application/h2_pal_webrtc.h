#ifndef H2_PAL_WEBRTC_H
#define H2_PAL_WEBRTC_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/core/h2_pal_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_pal_webrtc_peer h2_pal_webrtc_peer_t;
typedef struct h2_pal_webrtc_channel h2_pal_webrtc_channel_t;
typedef struct h2_pal_webrtc_track h2_pal_webrtc_track_t;

#define H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE 1275u

typedef enum h2_pal_webrtc_peer_state {
    H2_PAL_WEBRTC_PEER_NEW = 0,
    H2_PAL_WEBRTC_PEER_CONNECTING = 1,
    H2_PAL_WEBRTC_PEER_CONNECTED = 2,
    H2_PAL_WEBRTC_PEER_DISCONNECTED = 3,
    H2_PAL_WEBRTC_PEER_FAILED = 4,
    H2_PAL_WEBRTC_PEER_CLOSED = 5,
} h2_pal_webrtc_peer_state_t;

typedef enum h2_pal_webrtc_sdp_type {
    H2_PAL_WEBRTC_SDP_OFFER = 1,
    H2_PAL_WEBRTC_SDP_ANSWER = 2,
} h2_pal_webrtc_sdp_type_t;

typedef enum h2_pal_webrtc_channel_state {
    H2_PAL_WEBRTC_CHANNEL_OPEN = 1,
    H2_PAL_WEBRTC_CHANNEL_CLOSED = 2,
    H2_PAL_WEBRTC_CHANNEL_ERROR = 3,
} h2_pal_webrtc_channel_state_t;

typedef struct h2_pal_webrtc_str {
    const char *data;
    size_t len;
} h2_pal_webrtc_str_t;

typedef struct h2_pal_webrtc_channel_config {
    h2_pal_webrtc_str_t label;
    uint16_t stream_id;
    int has_stream_id;
    int ordered;
    int reliable;
} h2_pal_webrtc_channel_config_t;

typedef struct h2_pal_webrtc_ice_server {
    h2_pal_webrtc_str_t url;
    h2_pal_webrtc_str_t username;
    h2_pal_webrtc_str_t credential;
} h2_pal_webrtc_ice_server_t;

typedef struct h2_pal_webrtc_channel_info {
    h2_pal_webrtc_str_t label;
    uint16_t stream_id;
    int has_stream_id;
    int ordered;
    int reliable;
} h2_pal_webrtc_channel_info_t;

typedef void (*h2_pal_webrtc_peer_state_fn)(void *user,
                                            h2_pal_webrtc_peer_t *peer,
                                            h2_pal_webrtc_peer_state_t state);

typedef void (*h2_pal_webrtc_local_sdp_fn)(void *user,
                                           h2_pal_webrtc_peer_t *peer,
                                           h2_pal_webrtc_sdp_type_t type,
                                           h2_pal_webrtc_str_t sdp);

/*
 * The channel and info views are borrowed for this callback. CLOSED and ERROR
 * are terminal: the channel handle becomes invalid when the callback returns.
 */
typedef void (*h2_pal_webrtc_channel_state_fn)(
    void *user, h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_channel_t *channel,
    const h2_pal_webrtc_channel_info_t *info,
    h2_pal_webrtc_channel_state_t state);

typedef void (*h2_pal_webrtc_channel_message_fn)(
    void *user, h2_pal_webrtc_peer_t *peer, h2_pal_webrtc_channel_t *channel,
    const h2_pal_webrtc_channel_info_t *info, const uint8_t *data, size_t len,
    int is_text);

/*
 * Delivers one received Opus packet. RTP-aware providers report one missing
 * packet as opus == NULL and opus_len == 0 so codec owners can run packet-loss
 * concealment without collapsing the playback timeline.
 */
typedef void (*h2_pal_webrtc_opus_frame_fn)(void *user,
                                            h2_pal_webrtc_peer_t *peer,
                                            const uint8_t *opus,
                                            size_t opus_len);

typedef enum h2_pal_webrtc_receive_flags {
    H2_PAL_WEBRTC_RECEIVE_CALLBACKS = 0,
    H2_PAL_WEBRTC_RECEIVE_CHANNEL_PULL = 1u << 0,
    H2_PAL_WEBRTC_RECEIVE_OPUS_PULL = 1u << 1,
} h2_pal_webrtc_receive_flags_t;

typedef struct h2_pal_webrtc_callbacks {
    void *user;
    h2_pal_webrtc_peer_state_fn on_peer_state;
    h2_pal_webrtc_local_sdp_fn on_local_sdp;
    h2_pal_webrtc_channel_state_fn on_channel_state;
    h2_pal_webrtc_channel_message_fn on_channel_message;
    h2_pal_webrtc_opus_frame_fn on_opus_frame;
} h2_pal_webrtc_callbacks_t;

typedef struct h2_pal_webrtc_vtable {
    h2_pal_result_t (*peer_create)(void *user,
                                   const h2_pal_webrtc_callbacks_t *callbacks,
                                   h2_pal_webrtc_peer_t **out_peer);
    h2_pal_result_t (*peer_add_ice_server)(
        h2_pal_webrtc_peer_t *peer, const h2_pal_webrtc_ice_server_t *server);
    h2_pal_result_t (*peer_start_offer)(h2_pal_webrtc_peer_t *peer);
    h2_pal_result_t (*peer_set_remote_sdp)(h2_pal_webrtc_peer_t *peer,
                                           h2_pal_webrtc_sdp_type_t type,
                                           h2_pal_webrtc_str_t sdp);
    h2_pal_result_t (*peer_create_data_channel)(
        h2_pal_webrtc_peer_t *peer,
        const h2_pal_webrtc_channel_config_t *config,
        h2_pal_webrtc_channel_t **out_channel);
    h2_pal_result_t (*peer_set_media_track)(h2_pal_webrtc_peer_t *peer,
                                            h2_pal_webrtc_track_t *track);
    h2_pal_result_t (*peer_poll)(h2_pal_webrtc_peer_t *peer, int timeout_ms);
    h2_pal_result_t (*peer_send_opus)(h2_pal_webrtc_peer_t *peer,
                                      const uint8_t *opus, size_t opus_len);
    h2_pal_result_t (*channel_send)(h2_pal_webrtc_channel_t *channel,
                                    const uint8_t *data, size_t len,
                                    int is_text);
    void (*channel_close)(h2_pal_webrtc_channel_t *channel);
    void (*peer_close)(h2_pal_webrtc_peer_t *peer);
    h2_pal_result_t (*peer_create_pull)(
        void *user, const h2_pal_webrtc_callbacks_t *callbacks,
        uint32_t receive_flags, h2_pal_webrtc_peer_t **out_peer);
    h2_pal_result_t (*peer_receive_opus)(h2_pal_webrtc_peer_t *peer,
                                         uint8_t *opus, size_t opus_capacity,
                                         size_t *out_opus_len,
                                         uint32_t timeout_ms);
    h2_pal_result_t (*channel_receive)(h2_pal_webrtc_channel_t *channel,
                                       uint8_t *data, size_t capacity,
                                       size_t *out_len, int *out_is_text,
                                       uint32_t timeout_ms);
} h2_pal_webrtc_vtable_t;

typedef struct h2_pal_webrtc_api {
    void *user;
    const h2_pal_webrtc_vtable_t *vtable;
} h2_pal_webrtc_api_t;

static inline h2_pal_result_t
h2_pal_webrtc_peer_create(const h2_pal_webrtc_api_t *api,
                          const h2_pal_webrtc_callbacks_t *callbacks,
                          h2_pal_webrtc_peer_t **out_peer) {
    if (api == NULL || api->vtable == NULL ||
        api->vtable->peer_create == NULL || callbacks == NULL ||
        out_peer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->peer_create(api->user, callbacks, out_peer);
}

static inline h2_pal_result_t h2_pal_webrtc_peer_create_pull(
    const h2_pal_webrtc_api_t *api, const h2_pal_webrtc_callbacks_t *callbacks,
    uint32_t receive_flags, h2_pal_webrtc_peer_t **out_peer) {
    if (api == NULL || api->vtable == NULL || callbacks == NULL ||
        out_peer == NULL || receive_flags == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api->vtable->peer_create_pull == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->peer_create_pull(api->user, callbacks, receive_flags,
                                         out_peer);
}

/*
 * Adds one ICE server before peer_start_offer(). String views are borrowed
 * only for this synchronous call; the backend owns any storage it needs.
 */
static inline h2_pal_result_t
h2_pal_webrtc_peer_add_ice_server(const h2_pal_webrtc_api_t *api,
                                  h2_pal_webrtc_peer_t *peer,
                                  const h2_pal_webrtc_ice_server_t *server) {
    if (api == NULL || api->vtable == NULL ||
        api->vtable->peer_add_ice_server == NULL || peer == NULL ||
        server == NULL || server->url.data == NULL || server->url.len == 0u ||
        (server->username.data == NULL && server->username.len != 0u) ||
        (server->credential.data == NULL && server->credential.len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->peer_add_ice_server(peer, server);
}

static inline h2_pal_result_t
h2_pal_webrtc_peer_start_offer(const h2_pal_webrtc_api_t *api,
                               h2_pal_webrtc_peer_t *peer) {
    if (api == NULL || api->vtable == NULL ||
        api->vtable->peer_start_offer == NULL || peer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->peer_start_offer(peer);
}

static inline h2_pal_result_t h2_pal_webrtc_peer_set_remote_sdp(
    const h2_pal_webrtc_api_t *api, h2_pal_webrtc_peer_t *peer,
    h2_pal_webrtc_sdp_type_t type, h2_pal_webrtc_str_t sdp) {
    if (api == NULL || api->vtable == NULL ||
        api->vtable->peer_set_remote_sdp == NULL || peer == NULL ||
        sdp.data == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->peer_set_remote_sdp(peer, type, sdp);
}

/*
 * Creates one backend-owned channel handle for exclusive caller use. When
 * has_stream_id is false, the backend selects an available local stream ID.
 * H2_PAL_ERR_NO_SPACE means the bounded local stream-ID pool has no reusable
 * entry yet.
 */
static inline h2_pal_result_t h2_pal_webrtc_peer_create_data_channel(
    const h2_pal_webrtc_api_t *api, h2_pal_webrtc_peer_t *peer,
    const h2_pal_webrtc_channel_config_t *config,
    h2_pal_webrtc_channel_t **out_channel) {
    if (api == NULL || api->vtable == NULL ||
        api->vtable->peer_create_data_channel == NULL || peer == NULL ||
        config == NULL || out_channel == NULL || config->label.data == NULL ||
        config->label.len == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->peer_create_data_channel(peer, config, out_channel);
}

/*
 * Binds one provider-owned bidirectional media track before start_offer().
 * The track layout is private to the provider that created it; passing a
 * track to a different provider returns H2_PAL_ERR_INVALID_ARG. The caller
 * retains the handle, but it must outlive the peer and cannot be bound to two
 * live peers. A NULL track explicitly selects a data-only peer.
 *
 * Media capture, playback, codec work, RTP progression, and media-event
 * dispatch are provider responsibilities driven by peer_poll() or by the
 * provider's native event loop. Portable callers never read or write codec
 * packets through this handle.
 */
static inline h2_pal_result_t
h2_pal_webrtc_peer_set_media_track(const h2_pal_webrtc_api_t *api,
                                   h2_pal_webrtc_peer_t *peer,
                                   h2_pal_webrtc_track_t *track) {
    if (api == NULL || api->vtable == NULL || peer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api->vtable->peer_set_media_track == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->peer_set_media_track(peer, track);
}

static inline h2_pal_result_t
h2_pal_webrtc_peer_poll(const h2_pal_webrtc_api_t *api,
                        h2_pal_webrtc_peer_t *peer, int timeout_ms) {
    if (api == NULL || api->vtable == NULL || api->vtable->peer_poll == NULL ||
        peer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->peer_poll(peer, timeout_ms);
}

/*
 * Sends one complete raw Opus packet. The input is borrowed for this
 * synchronous call. H2_PAL_ERR_WOULD_BLOCK consumes no bytes and the caller
 * must retry the same packet before advancing the media stream.
 */
static inline h2_pal_result_t
h2_pal_webrtc_peer_send_opus(const h2_pal_webrtc_api_t *api,
                             h2_pal_webrtc_peer_t *peer, const uint8_t *opus,
                             size_t opus_len) {
    if (api == NULL || api->vtable == NULL ||
        api->vtable->peer_send_opus == NULL || peer == NULL || opus == NULL ||
        opus_len == 0u || opus_len > H2_PAL_WEBRTC_OPUS_MAX_PACKET_SIZE) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->peer_send_opus(peer, opus, opus_len);
}

/*
 * Receives one complete raw Opus packet in pull mode. Pull mode is selected
 * with H2_PAL_WEBRTC_RECEIVE_OPUS_PULL and a NULL on_opus_frame callback.
 * H2_PAL_OK with out_opus_len == 0 reports one missing RTP packet so the
 * caller can run packet-loss concealment. H2_PAL_ERR_NO_SPACE retains a real
 * packet and reports its required size in out_opus_len.
 */
static inline h2_pal_result_t h2_pal_webrtc_peer_receive_opus(
    const h2_pal_webrtc_api_t *api, h2_pal_webrtc_peer_t *peer, uint8_t *opus,
    size_t opus_capacity, size_t *out_opus_len, uint32_t timeout_ms) {
    if (api == NULL || api->vtable == NULL || peer == NULL || opus == NULL ||
        opus_capacity == 0u || out_opus_len == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api->vtable->peer_receive_opus == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->peer_receive_opus(peer, opus, opus_capacity,
                                          out_opus_len, timeout_ms);
}

/**
 * Sends one complete DataChannel message. The input is borrowed for this
 * synchronous call. H2_PAL_ERR_WOULD_BLOCK consumes no bytes and the caller
 * must retry the same message before advancing the stream.
 */
static inline h2_pal_result_t
h2_pal_webrtc_channel_send(const h2_pal_webrtc_api_t *api,
                           h2_pal_webrtc_channel_t *channel,
                           const uint8_t *data, size_t len, int is_text) {
    if (api == NULL || api->vtable == NULL ||
        api->vtable->channel_send == NULL || channel == NULL ||
        (data == NULL && len != 0u)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->channel_send(channel, data, len, is_text);
}

/*
 * Receives one complete DataChannel message in per-channel pull mode. Pull mode
 * is selected with H2_PAL_WEBRTC_RECEIVE_CHANNEL_PULL and a NULL
 * on_channel_message callback.
 * H2_PAL_ERR_NO_SPACE retains the message and reports its required size in
 * out_len. One caller exclusively owns receive operations for a channel.
 */
static inline h2_pal_result_t
h2_pal_webrtc_channel_receive(const h2_pal_webrtc_api_t *api,
                              h2_pal_webrtc_channel_t *channel, uint8_t *data,
                              size_t capacity, size_t *out_len,
                              int *out_is_text, uint32_t timeout_ms) {
    if (api == NULL || api->vtable == NULL || channel == NULL || data == NULL ||
        capacity == 0u || out_len == NULL || out_is_text == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api->vtable->channel_receive == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->channel_receive(channel, data, capacity, out_len,
                                        out_is_text, timeout_ms);
}

/*
 * Starts DataChannel close and consumes the handle. The handle is invalid when
 * this call returns. Any terminal callback, including one emitted synchronously
 * by this call, exposes only a borrowed view.
 */
static inline void
h2_pal_webrtc_channel_close(const h2_pal_webrtc_api_t *api,
                            h2_pal_webrtc_channel_t *channel) {
    if (api != NULL && api->vtable != NULL &&
        api->vtable->channel_close != NULL && channel != NULL) {
        api->vtable->channel_close(channel);
    }
}

static inline void h2_pal_webrtc_peer_close(const h2_pal_webrtc_api_t *api,
                                            h2_pal_webrtc_peer_t *peer) {
    if (api != NULL && api->vtable != NULL && api->vtable->peer_close != NULL &&
        peer != NULL) {
        api->vtable->peer_close(peer);
    }
}

#ifdef __cplusplus
}
#endif

#endif
