#ifndef H2_WEBRTC_COMPAT_FACTORY_H
#define H2_WEBRTC_COMPAT_FACTORY_H

#include "h2/pal/application/h2_pal_webrtc.h"

typedef struct h2_webrtc_compat_backend {
    const h2_pal_webrtc_api_t *api;
    void *state;
    void (*destroy)(void *state);
    const char *name;
    int supports_turn;
    int supports_channel_reuse;
    int supports_ice_tcp;
} h2_webrtc_compat_backend_t;

h2_pal_result_t h2_webrtc_compat_backend_create(
    h2_webrtc_compat_backend_t *out_backend);

#endif
