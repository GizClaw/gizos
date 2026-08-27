#include "h2_webrtc_compat_factory.h"

#include "h2_desktop_app_support_c.h"
#include "h2_peer.h"

#include <string.h>

static void h2_webrtc_h2peer_destroy(void *opaque) {
    h2_desktop_network_services_destroy(opaque);
}

static h2_pal_result_t h2_webrtc_h2peer_track_create(
    const h2_pal_webrtc_api_t *api, void *user,
    h2_webrtc_compat_track_read_fn read,
    h2_webrtc_compat_track_write_fn write,
    h2_pal_webrtc_track_t **out_track) {
    const h2_peer_media_track_config_t config = {
        .user = user,
        .read = read,
        .write = write,
    };
    return h2_peer_media_track_create((h2_peer_t *)api->user, &config,
                                      out_track);
}

h2_pal_result_t h2_webrtc_compat_backend_create(
    h2_webrtc_compat_backend_t *out_backend) {
    if (out_backend == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    memset(out_backend, 0, sizeof(*out_backend));
    h2_desktop_network_services_t *state = NULL;
    h2_pal_result_t result =
        h2_desktop_network_services_create(0, 1, &state);
    if (result != H2_PAL_OK) {
        return result;
    }
    out_backend->api = h2_desktop_network_services_webrtc(state);
    out_backend->state = state;
    out_backend->destroy = h2_webrtc_h2peer_destroy;
    out_backend->track_create = h2_webrtc_h2peer_track_create;
    out_backend->track_destroy = h2_peer_media_track_destroy;
    out_backend->name = "h2peer";
    out_backend->supports_turn = 1;
    out_backend->supports_channel_reuse = 1;
    out_backend->supports_ice_tcp = 1;
    return H2_PAL_OK;
}
