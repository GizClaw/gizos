#ifndef H2_PION_H
#define H2_PION_H

#include "h2/pal/application/h2_pal_webrtc.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque owner of the Go/Pion WebRTC PAL provider. */
typedef struct h2_pion h2_pion_t;

/** Dependencies borrowed for the complete provider lifetime. The allocator
 * must additionally outlive every outstanding owned event. */
typedef struct h2_pion_config {
  const h2_pal_mem_api_t *mem;
  const h2_pal_sync_api_t *sync;
  const h2_pal_task_api_t *task;
  const h2_pal_time_api_t *time;
} h2_pion_config_t;

/**
 * Creates a reusable Go/Pion WebRTC PAL provider.
 *
 * Each peer requires a PAL worker in addition to Pion's network goroutines.
 * Track read/write run on that worker; peer_poll only retrieves owned events.
 * Track methods must be nonblocking and must not call peer/provider lifecycle
 * methods. unset_track waits for in-flight Track access before returning.
 * Peer close/provider destroy must be serialized with other public API calls.
 */
h2_pal_result_t h2_pion_create(const h2_pion_config_t *config,
                               h2_pion_t **out_provider);

/** Returns a WebRTC PAL API borrowed until h2_pion_destroy(). */
const h2_pal_webrtc_api_t *h2_pion_webrtc_api(h2_pion_t *provider);

#if defined(H2_PION_TESTING) && H2_PION_TESTING
/** Test-only: reject the next Track Opus submission with WOULD_BLOCK. */
void h2_pion_test_block_next_opus_send(h2_pal_webrtc_peer_t *peer);
/** Test-only: return Track Opus submissions observed after the block setup. */
size_t h2_pion_test_opus_send_attempts(h2_pal_webrtc_peer_t *peer);
/** Test-only: report whether all observed submissions matched the first. */
int h2_pion_test_opus_send_payloads_match(h2_pal_webrtc_peer_t *peer);
/** Test-only bridge injection serialized with the real worker. */
h2_pal_result_t h2_pion_test_connected(h2_pal_webrtc_peer_t *peer);
h2_pal_result_t h2_pion_test_remote_channel(h2_pal_webrtc_peer_t *peer);
#endif

/** Closes all peers and destroys the provider. NULL is accepted. A failed task
 * join retains *provider so destruction can be retried without freeing a live
 * worker's dependencies. */
void h2_pion_destroy(h2_pion_t **provider);

#ifdef __cplusplus
}
#endif

#endif
