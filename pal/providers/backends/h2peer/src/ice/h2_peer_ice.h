#ifndef H2_PEER_ICE_H
#define H2_PEER_ICE_H

#include "core/h2_pal_errors.h"
#include "application/h2_pal_webrtc.h"

h2_pal_result_t h2_peer_ice_validate_server(
    const h2_pal_webrtc_ice_server_t* server);

#endif
