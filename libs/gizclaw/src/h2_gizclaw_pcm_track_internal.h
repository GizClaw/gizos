#ifndef H2_GIZCLAW_PCM_TRACK_INTERNAL_H
#define H2_GIZCLAW_PCM_TRACK_INTERNAL_H

#include "h2_gizclaw_pcm_track.h"

/* Private audio-port implementation. The public Track is opaque; application
 * code cannot supply callbacks. Fault-injection tests use this boundary. */
typedef struct h2_gizclaw_track_vtable {
  h2_pal_result_t (*read)(void *user, uint8_t *pcm, size_t capacity,
                        size_t *out_len);
  h2_pal_result_t (*write)(void *user, const uint8_t *pcm, size_t len);
} h2_gizclaw_track_vtable_t;

struct h2_gizclaw_track {
  void *user;
  const h2_gizclaw_track_vtable_t *vtable;
};

/* Service binding pins library-owned Tracks until all accesses have ended. */
h2_pal_result_t h2_gizclaw_pcm_track_attach_internal(h2_gizclaw_track_t *track);
void h2_gizclaw_pcm_track_detach_internal(h2_gizclaw_track_t *track);
/* Drop downlink PCM queued before now. Called by the writer side when a new
 * audio request starts or the current one is cancelled; applied by the
 * consumer on its next read. */
void h2_gizclaw_pcm_track_discard_downlink_internal(h2_gizclaw_track_t *track);
/* Snapshot only while the single uplink consumer is paused. */
size_t h2_gizclaw_pcm_track_pending_internal(h2_gizclaw_track_t *track);

/* One recording window. Control and consumer access are serialized by the
 * route's input mutex. Only prepare/read, on the uplink task, move read_index. */
typedef struct h2_gizclaw_pcm_input {
  h2_gizclaw_track_t *track;
  size_t begin, end;
  uint8_t *tail;
  const h2_pal_mem_api_t *tail_allocator;
  size_t tail_len, tail_offset;
  bool active, ended, begun, tail_taken;
} h2_gizclaw_pcm_input_t;

h2_pal_result_t h2_gizclaw_pcm_input_start(h2_gizclaw_pcm_input_t *input,
                                          h2_gizclaw_track_t *track,
                                          const h2_pal_mem_api_t *allocator);
h2_pal_result_t h2_gizclaw_pcm_input_end(h2_gizclaw_pcm_input_t *input,
                                        h2_gizclaw_track_t *track);
h2_pal_result_t h2_gizclaw_pcm_input_prepare(h2_gizclaw_pcm_input_t *input,
                                            h2_gizclaw_track_t *track);
h2_pal_result_t h2_gizclaw_pcm_input_read(h2_gizclaw_pcm_input_t *input,
    h2_gizclaw_track_t *track, uint8_t *pcm, size_t capacity, size_t *out_len);
void h2_gizclaw_pcm_input_deinit(h2_gizclaw_pcm_input_t *input);

#endif
