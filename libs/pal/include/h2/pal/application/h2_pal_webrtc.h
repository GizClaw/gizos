#ifndef H2_PAL_WEBRTC_H
#define H2_PAL_WEBRTC_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/core/h2_pal_types.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_pal_webrtc_peer h2_pal_webrtc_peer_t;
typedef struct h2_pal_webrtc_channel h2_pal_webrtc_channel_t;
typedef struct h2_pal_webrtc_event h2_pal_webrtc_event_t;

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

typedef h2_pal_result_t (*h2_pal_webrtc_track_read_fn)(void *user,
                                                       uint8_t *opus,
                                                       size_t capacity,
                                                       size_t *out_len);

typedef h2_pal_result_t (*h2_pal_webrtc_track_write_fn)(void *user,
                                                        const uint8_t *opus,
                                                        size_t opus_len);

typedef struct h2_pal_webrtc_track_vtable {
    h2_pal_webrtc_track_read_fn read;
    h2_pal_webrtc_track_write_fn write;
} h2_pal_webrtc_track_vtable_t;

/*
 * Caller-owned bidirectional media track. The provider borrows this object
 * between peer_set_track() and peer_unset_track(). Native providers use the
 * vtable; browser providers may instead use native_handle as an opaque token.
 */
typedef struct h2_pal_webrtc_track {
    void *user;
    const h2_pal_webrtc_track_vtable_t *vtable;
    void *native_handle;
} h2_pal_webrtc_track_t;

typedef enum h2_pal_webrtc_event_kind {
    H2_PAL_WEBRTC_EVENT_PEER_STATE = 1,
    H2_PAL_WEBRTC_EVENT_LOCAL_SDP = 2,
    H2_PAL_WEBRTC_EVENT_CHANNEL_STATE = 3,
    H2_PAL_WEBRTC_EVENT_CHANNEL_MESSAGE = 4,
    H2_PAL_WEBRTC_EVENT_OPUS_FRAME = 5,
    H2_PAL_WEBRTC_EVENT_WRITABLE = 6,
    H2_PAL_WEBRTC_EVENT_ERROR = 7,
} h2_pal_webrtc_event_kind_t;

typedef void (*h2_pal_webrtc_event_release_fn)(h2_pal_webrtc_event_t *event);

/*
 * One owned event returned by peer_poll(). Payload views and channel metadata
 * remain valid until h2_pal_webrtc_event_release() is called. A zero-length
 * OPUS_FRAME represents one packet-loss marker. The peer and channel pointers
 * identify the event source; after either handle is closed they must not be
 * dereferenced or passed back to the provider.
 */
struct h2_pal_webrtc_event {
    h2_pal_webrtc_event_kind_t kind;
    h2_pal_webrtc_peer_t *peer;
    h2_pal_webrtc_channel_t *channel;
    h2_pal_webrtc_channel_info_t channel_info;
    h2_pal_webrtc_peer_state_t peer_state;
    h2_pal_webrtc_channel_state_t channel_state;
    h2_pal_webrtc_sdp_type_t sdp_type;
    h2_pal_webrtc_str_t sdp;
    const uint8_t *data;
    size_t data_len;
    int is_text;
    h2_pal_result_t error;
    void *_private;
    h2_pal_webrtc_event_release_fn _release;
};

typedef struct h2_pal_webrtc_vtable {
  h2_pal_result_t (*peer_create)(void *user, h2_pal_webrtc_peer_t **out_peer);
  h2_pal_result_t (*peer_add_ice_server)(
      h2_pal_webrtc_peer_t *peer, const h2_pal_webrtc_ice_server_t *server);
  h2_pal_result_t (*peer_start_offer)(h2_pal_webrtc_peer_t *peer);
  h2_pal_result_t (*peer_set_remote_sdp)(h2_pal_webrtc_peer_t *peer,
                                         h2_pal_webrtc_sdp_type_t type,
                                         h2_pal_webrtc_str_t sdp);
  h2_pal_result_t (*peer_create_data_channel)(
      h2_pal_webrtc_peer_t *peer, const h2_pal_webrtc_channel_config_t *config,
      h2_pal_webrtc_channel_t **out_channel);
  h2_pal_result_t (*peer_set_track)(h2_pal_webrtc_peer_t *peer,
                                    h2_pal_webrtc_track_t *track);
  h2_pal_result_t (*peer_unset_track)(h2_pal_webrtc_peer_t *peer,
                                      h2_pal_webrtc_track_t *track);
  h2_pal_result_t (*peer_poll)(h2_pal_webrtc_peer_t *peer, int timeout_ms,
                               h2_pal_webrtc_event_t *out_event);
  h2_pal_result_t (*peer_send_opus)(h2_pal_webrtc_peer_t *peer,
                                    const uint8_t *opus, size_t opus_len);
  h2_pal_result_t (*channel_send)(h2_pal_webrtc_channel_t *channel,
                                  const uint8_t *data, size_t len, int is_text);
  void (*channel_close)(h2_pal_webrtc_channel_t *channel);
  void (*peer_close)(h2_pal_webrtc_peer_t *peer);
} h2_pal_webrtc_vtable_t;

typedef struct h2_pal_webrtc_api {
    void *user;
    const h2_pal_webrtc_vtable_t *vtable;
} h2_pal_webrtc_api_t;

static inline h2_pal_result_t
h2_pal_webrtc_peer_create(const h2_pal_webrtc_api_t *api,
                          h2_pal_webrtc_peer_t **out_peer) {
  if (api == NULL || api->vtable == NULL || api->vtable->peer_create == NULL ||
      out_peer == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    return api->vtable->peer_create(api->user, out_peer);
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
 * Binds a caller-owned bidirectional media track before start_offer(). The
 * provider borrows the track until peer_unset_track() succeeds.
 */
static inline h2_pal_result_t
h2_pal_webrtc_peer_set_track(const h2_pal_webrtc_api_t *api,
                             h2_pal_webrtc_peer_t *peer,
                             h2_pal_webrtc_track_t *track) {
    if (api == NULL || api->vtable == NULL || peer == NULL || track == NULL ||
        (track->vtable == NULL && track->native_handle == NULL)) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api->vtable->peer_set_track == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->peer_set_track(peer, track);
}

static inline h2_pal_result_t
h2_pal_webrtc_peer_unset_track(const h2_pal_webrtc_api_t *api,
                               h2_pal_webrtc_peer_t *peer,
                               h2_pal_webrtc_track_t *track) {
    if (api == NULL || api->vtable == NULL || peer == NULL || track == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api->vtable->peer_unset_track == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->peer_unset_track(peer, track);
}

static inline h2_pal_result_t
h2_pal_webrtc_peer_poll(const h2_pal_webrtc_api_t *api,
                        h2_pal_webrtc_peer_t *peer, int timeout_ms,
                        h2_pal_webrtc_event_t *out_event) {
    if (api == NULL || api->vtable == NULL || api->vtable->peer_poll == NULL ||
        peer == NULL || out_event == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_event, 0, sizeof(*out_event));
    return api->vtable->peer_poll(peer, timeout_ms, out_event);
}

static inline void h2_pal_webrtc_event_release(h2_pal_webrtc_event_t *event) {
    if (event == NULL) {
        return;
    }
    if (event->_release != NULL) {
        event->_release(event);
    } else {
        memset(event, 0, sizeof(*event));
    }
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
 * Starts DataChannel close and consumes the handle. The caller must not use the
 * handle again. A later terminal event may carry the pointer only as an
 * identity; its copied channel_info remains valid until event release.
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
