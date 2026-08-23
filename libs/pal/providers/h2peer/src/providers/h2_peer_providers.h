#ifndef H2_PEER_PROVIDERS_H
#define H2_PEER_PROVIDERS_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/net/h2_pal_sctp.h"
#include "h2/pal/application/h2_pal_webrtc.h"

#include <stddef.h>
#include <stdint.h>

typedef struct h2_peer_ice_provider_vtable {
    h2_pal_result_t (*open)(
        void *user,
        const h2_pal_webrtc_ice_server_t *servers,
        size_t server_count,
        void **out_session);
    h2_pal_result_t (*poll)(void *user, void *session, int timeout_ms);
    void (*close)(void *user, void *session);
} h2_peer_ice_provider_vtable_t;

typedef struct h2_peer_ice_provider {
    void *user;
    const h2_peer_ice_provider_vtable_t *vtable;
} h2_peer_ice_provider_t;

typedef struct h2_peer_dtls_provider_vtable {
    h2_pal_result_t (*open)(void *user, void **out_session);
    h2_pal_result_t (*get_local_fingerprint)(
        void *user,
        void *session,
        char *out,
        size_t out_cap,
        size_t *out_len);
    h2_pal_result_t (*set_remote_fingerprint)(
        void *user,
        void *session,
        h2_pal_webrtc_str_t fingerprint);
    h2_pal_result_t (*poll)(void *user, void *session, int timeout_ms);
    void (*close)(void *user, void *session);
} h2_peer_dtls_provider_vtable_t;

typedef struct h2_peer_dtls_provider {
    void *user;
    const h2_peer_dtls_provider_vtable_t *vtable;
} h2_peer_dtls_provider_t;

typedef struct h2_peer_srtp_provider_vtable {
    h2_pal_result_t (*open)(void *user, void **out_session);
    h2_pal_result_t (*send_rtp)(
        void *user,
        void *session,
        const uint8_t *packet,
        size_t packet_len);
    h2_pal_result_t (*receive_rtp)(
        void *user,
        void *session,
        const uint8_t *packet,
        size_t packet_len,
        uint8_t *out_packet,
        size_t out_cap,
        size_t *out_len);
    void (*close)(void *user, void *session);
} h2_peer_srtp_provider_vtable_t;

typedef struct h2_peer_srtp_provider {
    void *user;
    const h2_peer_srtp_provider_vtable_t *vtable;
} h2_peer_srtp_provider_t;

typedef enum h2_peer_sctp_event_type {
    H2_PEER_SCTP_EVENT_CHANNEL_OPEN = 1,
    H2_PEER_SCTP_EVENT_CHANNEL_MESSAGE = 2,
    H2_PEER_SCTP_EVENT_CHANNEL_CLOSED = 3,
    H2_PEER_SCTP_EVENT_STREAM_RESET = 4,
} h2_peer_sctp_event_type_t;

typedef struct h2_peer_sctp_event {
    h2_peer_sctp_event_type_t type;
    uint16_t stream_id;
    const uint8_t *data;
    size_t data_len;
    int is_text;
    h2_pal_sctp_stream_reset_direction_t reset_direction;
    h2_pal_result_t reset_result;
} h2_peer_sctp_event_t;

typedef void (*h2_peer_sctp_event_fn)(
    void *user,
    const h2_peer_sctp_event_t *event);

typedef struct h2_peer_sctp_provider_vtable {
    h2_pal_result_t (*open)(void *user, void **out_session);
    h2_pal_result_t (*poll)(
        void *user,
        void *session,
        int timeout_ms,
        h2_peer_sctp_event_fn event_fn,
        void *event_user);
    h2_pal_result_t (*channel_open)(
        void *user,
        void *session,
        uint16_t stream_id,
        h2_pal_webrtc_str_t label,
        int ordered,
        int reliable);
    h2_pal_result_t (*send)(
        void *user,
        void *session,
        uint16_t stream_id,
        const uint8_t *data,
        size_t len,
        int is_text);
    h2_pal_result_t (*channel_close)(
        void *user,
        void *session,
        uint16_t stream_id);
    void (*close)(void *user, void *session);
} h2_peer_sctp_provider_vtable_t;

typedef struct h2_peer_sctp_provider {
    void *user;
    const h2_peer_sctp_provider_vtable_t *vtable;
} h2_peer_sctp_provider_t;

typedef struct h2_peer_provider_bundle {
    h2_peer_ice_provider_t ice;
    h2_peer_dtls_provider_t dtls;
    h2_peer_srtp_provider_t srtp;
    h2_peer_sctp_provider_t sctp;
} h2_peer_provider_bundle_t;

const h2_peer_provider_bundle_t *h2_peer_unavailable_providers(void);

#endif
