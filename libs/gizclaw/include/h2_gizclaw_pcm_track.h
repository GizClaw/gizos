#ifndef H2_GIZCLAW_PCM_TRACK_H
#define H2_GIZCLAW_PCM_TRACK_H

#include "h2_gizclaw_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 512 ms per direction at 16 kHz, PCM16 mono. */
#define H2_GIZCLAW_PCM_TRACK_DEFAULT_CAPACITY 16384u

/** Library-owned PCM storage. Zero capacity selects the 512 ms default;
 * overrides are powers of two, in bytes, and at least two (e.g. stress tests).
 * Samples are signed 16-bit little-endian mono. The Track has
 * no device, codec, timer, MIME type, or EOS; pumps own device pacing. */
typedef struct h2_gizclaw_pcm_track_config {
  const h2_pal_mem_api_t *allocator;
  size_t uplink_capacity;
  size_t downlink_capacity;
} h2_gizclaw_pcm_track_config_t;

h2_pal_result_t
h2_gizclaw_pcm_track_create(const h2_gizclaw_pcm_track_config_t *config,
                            h2_gizclaw_track_t **out_track);

/** Stop the external pumps and unset the Track (or deinit its Service) first.
 * Returns BUSY while bound to a Service. Must not race with any Track access.
 * On success sets *track to NULL; a NULL *track is already destroyed. */
h2_pal_result_t h2_gizclaw_pcm_track_destroy(h2_gizclaw_track_t **track);

/** Application/mic producer: append to uplink. One producer only.
 * All-or-nothing, nonblocking: WOULD_BLOCK leaves bytes and cursors unchanged.
 * Length must be nonzero, sample aligned, and no larger than capacity. */
h2_pal_result_t h2_gizclaw_pcm_track_write(h2_gizclaw_track_t *track,
                                           const uint8_t *pcm, size_t len);

/** Application/speaker consumer: read exactly len bytes from downlink.
 * One consumer only. WOULD_BLOCK does not consume a partial frame or change
 * the output buffer. Length obeys the same alignment/capacity constraints. */
h2_pal_result_t h2_gizclaw_pcm_track_read(h2_gizclaw_track_t *track,
                                          uint8_t *pcm, size_t len);

#ifdef __cplusplus
}
#endif

#endif
