#ifndef H2_APP_TEST_WEBRTC_H
#define H2_APP_TEST_WEBRTC_H
#include "h2/pal/application/h2_pal_webrtc.h"
#include "h2/pal/os/h2_pal_mem.h"
#ifdef __cplusplus
extern "C" {
#endif
#define H2_APP_TEST_WEBRTC_CHANNELS_MAX 16u
typedef struct h2_app_test_webrtc h2_app_test_webrtc_t;
/** Called synchronously after a successful poll. Event is borrowed and must not
 * be released or retained by this callback. Callbacks on different peers may
 * overlap; the consumer serializes its observations. No protocol is assumed. */
typedef void (*h2_app_test_webrtc_observe_fn)(
    void *user, const h2_pal_webrtc_event_t *event);
/** Decorate a real or fake provider, preserving errors, payload ownership and
 * track handles. Peer/channel handles belong to this decorator and must only be
 * used with its API. Each peer retains at most CHANNELS_MAX distinct channel
 * handles until peer close. Caller serializes operations on each peer and its
 * channels, and releases events before closing their peer. Dependencies and
 * callback context remain borrowed until destroy. */
h2_pal_result_t h2_app_test_webrtc_create(const h2_pal_mem_api_t *mem,
                                          const h2_pal_webrtc_api_t *delegate,
                                          h2_app_test_webrtc_observe_fn observe,
                                          void *user,
                                          h2_app_test_webrtc_t **out);
const h2_pal_webrtc_api_t *
h2_app_test_webrtc_api(h2_app_test_webrtc_t *wrapper);
/** Requires quiescent callers. Active peers/events return INVALID_STATE and
 * retain ownership; NULL is OK. */
h2_pal_result_t h2_app_test_webrtc_destroy(h2_app_test_webrtc_t *wrapper);
#ifdef __cplusplus
}
#endif
#endif
