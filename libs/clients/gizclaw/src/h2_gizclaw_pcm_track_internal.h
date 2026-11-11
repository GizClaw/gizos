#ifndef H2_GIZCLAW_PCM_TRACK_INTERNAL_H
#define H2_GIZCLAW_PCM_TRACK_INTERNAL_H

#include "h2_gizclaw_pcm_track.h"

#include "h2_atomic.h"

/* Private audio-port implementation. The public Track is opaque; application
 * code cannot supply callbacks. Fault-injection tests use this boundary. */
typedef struct h2_gizclaw_track_vtable {
  h2_pal_result_t (*read)(void* user, uint8_t* pcm, size_t capacity,
                          size_t* out_len);
  h2_pal_result_t (*write)(void* user, const uint8_t* pcm, size_t len);
} h2_gizclaw_track_vtable_t;

struct h2_gizclaw_track {
  void* user;
  const h2_gizclaw_track_vtable_t* vtable;
};

/* Service binding pins library-owned Tracks until all accesses have ended.
 * Optional downlink_activity is the Service lifetime counter, borrowed until
 * detach. Bind before publishing the Track; quiesce managed captures and all
 * Service borrowers before detach. NULL selects a standalone Track counter. */
h2_pal_result_t h2_gizclaw_pcm_track_attach_internal(
    h2_gizclaw_track_t* track, h2_atomic_size_t* downlink_activity);
void h2_gizclaw_pcm_track_detach_internal(h2_gizclaw_track_t* track);
/* Owned Tracks commit the Service activity counter atomically with PCM writes
 * under the capture mutex. Custom Tracks require Service-side accounting. */
bool h2_gizclaw_pcm_track_owned_internal(h2_gizclaw_track_t* track);
/* Drop downlink PCM queued before now. Called by the writer side when a new
 * audio request starts or the current one is cancelled; applied by the
 * consumer on its next read. */
void h2_gizclaw_pcm_track_discard_downlink_internal(h2_gizclaw_track_t* track);
/* Snapshot only while the single uplink consumer is paused. */
size_t h2_gizclaw_pcm_track_pending_internal(h2_gizclaw_track_t* track);
/* Monotonic downlink byte counter; no Track mutex needed. Caller pins Track.
 * Non-owned Tracks return zero. */
size_t h2_gizclaw_pcm_track_downlink_writes_internal(h2_gizclaw_track_t* track);
/* True while a managed window or either lease prevents Track unbinding/reuse.
 * Non-owned Tracks return false. Mutex errors conservatively report busy. */
bool h2_gizclaw_pcm_capture_busy_internal(h2_gizclaw_track_t* track);

/* One recording window. The route's input mutex serializes this object.
 * Managed inputs borrow the Track until deinit; the Track mutex serializes
 * their ring access with capture control and producer frame submission.
 * Legacy inputs pin the ring until tail save or deinit. Saved tails own their
 * storage; prepare/read/deinit then need no live Track. Do not copy live
 * inputs. Deinit always clears the input: managed retirement is published to
 * Track-owned atomic state without locking, then collected on the next
 * successful Track lock. No Track access follows retirement publication. */
typedef struct h2_gizclaw_pcm_input {
  h2_gizclaw_track_t* track;
  size_t begin,
      end; /* Legacy ring watermarks; managed inputs use Track state. */
  size_t end_downlink_write_index; /* Managed producer-acknowledged END. */
  uint8_t* tail;
  const h2_pal_mem_api_t* tail_allocator;
  size_t tail_len, tail_offset;
  bool active, ended, begun, tail_taken;
  bool empty; /* Frozen at END for library-owned Tracks. */
  bool
      legacy_lease; /* Pins the Track only until legacy tail save/retirement. */
  h2_gizclaw_pcm_capture_id_t
      capture_id; /* Nonzero owns a managed reader lease. */
} h2_gizclaw_pcm_input_t;

/* Bind the original begin watermark, including pre-adoption PCM and FROZEN
 * captures. Accepts a zero input or an active legacy START placeholder on this
 * Track that has not begun or ended; otherwise INVALID_STATE. Re-adopting the
 * same input/id is idempotent. Another reader returns BUSY; stale IDs
 * NOT_FOUND. No tail allocation. prepare/read never reset the managed ring;
 * read returns WOULD_BLOCK while capturing/stopping without PCM, short reads
 * for available PCM, then OK/zero EOS only after producer acknowledgement.
 * Errors propagate. deinit must return this lease before begin/destroy, even
 * after EOS/failure. Deinit cancels an unfinished capture (waiting for any
 * producer lease), or releases/discards its frozen remainder. Legacy prepare
 * and read return WOULD_BLOCK while a managed window occupies the ring. */
h2_pal_result_t h2_gizclaw_pcm_input_adopt(h2_gizclaw_pcm_input_t* input,
                                           h2_gizclaw_track_t* track,
                                           const h2_pal_mem_api_t* allocator,
                                           h2_gizclaw_pcm_capture_id_t id);

h2_pal_result_t h2_gizclaw_pcm_input_start(h2_gizclaw_pcm_input_t* input,
                                           h2_gizclaw_track_t* track,
                                           const h2_pal_mem_api_t* allocator);
h2_pal_result_t h2_gizclaw_pcm_input_end(h2_gizclaw_pcm_input_t* input,
                                         h2_gizclaw_track_t* track);
h2_pal_result_t h2_gizclaw_pcm_input_prepare(h2_gizclaw_pcm_input_t* input,
                                             h2_gizclaw_track_t* track);
h2_pal_result_t h2_gizclaw_pcm_input_read(h2_gizclaw_pcm_input_t* input,
                                          h2_gizclaw_track_t* track,
                                          uint8_t* pcm, size_t capacity,
                                          size_t* out_len);
void h2_gizclaw_pcm_input_deinit(h2_gizclaw_pcm_input_t* input);

#endif
