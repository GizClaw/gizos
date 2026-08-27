#ifndef H2_PION_H
#define H2_PION_H

#include "h2/pal/application/h2_pal_webrtc.h"
#include "h2/pal/os/h2_pal_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque owner of the Go/Pion WebRTC PAL provider. */
typedef struct h2_pion h2_pion_t;

typedef h2_pal_result_t (*h2_pion_media_track_read_fn)(void *user,
                                                       uint8_t *opus,
                                                       size_t capacity,
                                                       size_t *out_len);
typedef h2_pal_result_t (*h2_pion_media_track_write_fn)(void *user,
                                                        const uint8_t *opus,
                                                        size_t len);

typedef struct h2_pion_media_track_config {
  void *user;
  h2_pion_media_track_read_fn read;
  h2_pion_media_track_write_fn write;
} h2_pion_media_track_config_t;

/** Dependencies borrowed for the complete provider lifetime. */
typedef struct h2_pion_config {
  const h2_pal_mem_api_t *mem;
} h2_pion_config_t;

/**
 * Creates a reusable Go/Pion WebRTC PAL provider.
 *
 * One provider may own multiple peers. Pion worker goroutines enqueue copied
 * events; PAL callbacks run only from the caller of peer_poll().
 */
h2_pal_result_t h2_pion_create(const h2_pion_config_t *config,
                               h2_pion_t **out_provider);

/** Returns a WebRTC PAL API borrowed until h2_pion_destroy(). */
const h2_pal_webrtc_api_t *h2_pion_webrtc_api(h2_pion_t *provider);

h2_pal_result_t
h2_pion_media_track_create(h2_pion_t *provider,
                           const h2_pion_media_track_config_t *config,
                           h2_pal_webrtc_track_t **out_track);
h2_pal_result_t h2_pion_media_track_destroy(h2_pal_webrtc_track_t **track);

#if defined(H2_PION_TESTING) && H2_PION_TESTING
/** Test-only: reject the next Track Opus submission with WOULD_BLOCK. */
void h2_pion_test_block_next_opus_send(h2_pal_webrtc_peer_t *peer);
/** Test-only: return Track Opus submissions observed after the block setup. */
size_t h2_pion_test_opus_send_attempts(h2_pal_webrtc_peer_t *peer);
/** Test-only: report whether all observed submissions matched the first. */
int h2_pion_test_opus_send_payloads_match(h2_pal_webrtc_peer_t *peer);
#endif

/** Closes all peers and destroys the provider. NULL is accepted. */
void h2_pion_destroy(h2_pion_t **provider);

#ifdef __cplusplus
}
#endif

#endif
