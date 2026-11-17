#ifndef H2_BK_WEBRTC_PERF_HOOKS_H
#define H2_BK_WEBRTC_PERF_HOOKS_H

/* Load the real PAL inlines before remapping just the scenario's call sites.
 * The observer implementation uses the original inlines without this remap. */
#include "application/h2_pal_webrtc.h"

h2_pal_result_t h2_bk_perf_observe_send_opus(const h2_pal_webrtc_api_t* api,
                                             h2_pal_webrtc_peer_t* peer,
                                             const uint8_t* opus,
                                             size_t opus_len);
h2_pal_result_t h2_bk_perf_observe_poll(const h2_pal_webrtc_api_t* api,
                                        h2_pal_webrtc_peer_t* peer,
                                        int timeout_ms,
                                        h2_pal_webrtc_event_t* out_event);

#if !defined(H2_BK_PERF_OBSERVER_IMPLEMENTATION)
#define h2_pal_webrtc_peer_send_opus h2_bk_perf_observe_send_opus
#define h2_pal_webrtc_peer_poll h2_bk_perf_observe_poll
#endif

#endif
