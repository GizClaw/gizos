#ifndef H2_WEBRTC_COMPAT_FACTORY_H
#define H2_WEBRTC_COMPAT_FACTORY_H

#include "h2/pal/application/h2_pal_webrtc.h"

typedef h2_pal_result_t (*h2_webrtc_compat_track_read_fn)(
    void *user, uint8_t *opus, size_t capacity, size_t *out_len);
typedef h2_pal_result_t (*h2_webrtc_compat_track_write_fn)(
    void *user, const uint8_t *opus, size_t len);

typedef struct h2_webrtc_compat_backend {
    const h2_pal_webrtc_api_t *api;
    void *state;
    void (*destroy)(void *state);
    h2_pal_result_t (*track_create)(
        const h2_pal_webrtc_api_t *api, void *user,
        h2_webrtc_compat_track_read_fn read,
        h2_webrtc_compat_track_write_fn write,
        h2_pal_webrtc_track_t **out_track);
    h2_pal_result_t (*track_destroy)(h2_pal_webrtc_track_t **track);
    const char *name;
    int supports_turn;
    int supports_channel_reuse;
    int supports_ice_tcp;
} h2_webrtc_compat_backend_t;

h2_pal_result_t h2_webrtc_compat_backend_create(
    h2_webrtc_compat_backend_t *out_backend);

#endif
