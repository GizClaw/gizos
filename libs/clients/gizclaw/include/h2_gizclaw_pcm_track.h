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
  const h2_pal_mem_api_t* allocator;
  size_t uplink_capacity;
  size_t downlink_capacity;
  /** Optional borrowed Mutex PAL; must outlive the Track. Without it all legacy
   * APIs remain available and capture APIs return UNSUPPORTED. Mutex operations
   * serialize short memory-only sections; no device read runs under the lock.
   */
  const h2_pal_sync_api_t* sync;
} h2_gizclaw_pcm_track_config_t;

typedef enum h2_gizclaw_pcm_capture_state {
  H2_GIZCLAW_PCM_CAPTURE_IDLE = 0,
  H2_GIZCLAW_PCM_CAPTURE_CAPTURING,
  H2_GIZCLAW_PCM_CAPTURE_STOPPING,
  H2_GIZCLAW_PCM_CAPTURE_FROZEN,
  H2_GIZCLAW_PCM_CAPTURE_DISCARDING,
  H2_GIZCLAW_PCM_CAPTURE_DISCARDED,
  H2_GIZCLAW_PCM_CAPTURE_DONE,
  H2_GIZCLAW_PCM_CAPTURE_FAILED,
} h2_gizclaw_pcm_capture_state_t;

typedef struct h2_gizclaw_pcm_capture_snapshot {
  h2_gizclaw_pcm_capture_id_t id;
  h2_gizclaw_pcm_capture_state_t state;
  bool adopted;
  bool producer_busy;
  bool consumer_busy;
  size_t pending_bytes;
  size_t captured_bytes;
  h2_pal_result_t result;
} h2_gizclaw_pcm_capture_snapshot_t;

h2_pal_result_t h2_gizclaw_pcm_track_create(
    const h2_gizclaw_pcm_track_config_t* config,
    h2_gizclaw_track_t** out_track);

/** Stop the external pumps and unset the Track (or deinit its Service) first.
 * Returns BUSY while bound, while a capture awaits completion/adoption, or
 * while a producer/managed reader lease remains, or while a legacy input still
 * borrows the ring. A saved legacy tail no longer pins the Track. Must not race
 * with any Track access. On success sets *track to NULL; a NULL *track is
 * already destroyed.
 */
h2_pal_result_t h2_gizclaw_pcm_track_destroy(h2_gizclaw_track_t** track);

/** Begin a local capture, discarding pre-begin ambient PCM. On success writes a
 * fresh nonzero Track-scoped identity to out_id (zeroed on failure). BUSY until
 * the previous capture is DONE, DISCARDED, or FAILED and both leases returned;
 * an unadopted FROZEN capture must be adopted or discarded first. Also BUSY
 * while a live legacy window has accepted PCM, begun reading, or ended, until
 * its tail is saved or its input retires. An untouched empty START placeholder
 * may be adopted after begin; its old watermark is invalidated. No PCM storage
 * is allocated: the window occupies the existing uplink ring.
 *
 * Capture APIs are task-context, thread-safe via the injected Mutex PAL, and do
 * not wait for device I/O. Missing sync returns UNSUPPORTED. Except snapshot,
 * zero IDs return INVALID_ARG; replaced IDs return NOT_FOUND without mutation.
 * IDs are never reused during a Track lifetime. */
h2_pal_result_t h2_gizclaw_pcm_capture_begin(
    h2_gizclaw_track_t* track, h2_gizclaw_pcm_capture_id_t* out_id);

/** Acquire one producer frame lease immediately before the PAL mic read. Only
 * CAPTURING accepts a lease; a second in-flight frame returns BUSY, and a
 * stopped capture returns CLOSED (FAILED returns its latched result). Every
 * successful acquire requires exactly one submit, including failed/empty PAL
 * reads. */
h2_pal_result_t h2_gizclaw_pcm_capture_frame_acquire(
    h2_gizclaw_track_t* track, h2_gizclaw_pcm_capture_id_t id);

/** Return the matching frame lease. Successful PCM is copied before return and
 * must be nonempty PCM16 mono. WOULD_BLOCK/TIMEOUT with NULL PCM and zero
 * length completes an empty frame without stopping capture; any other buffer
 * or length combination with those results latches INVALID_ARG. Other producer
 * failures, malformed PCM, or insufficient capacity latch FAILED and its first
 * result; saturation returns NO_SPACE without overwriting or partially
 * appending PCM. END accepts the one leased frame then freezes; DISCARD drops
 * it and succeeds. A matching lease is released even on error. Submitting
 * without a lease returns INVALID_STATE; a stale submit never releases a newer
 * capture's lease. After a synchronization failure, the matching lease return
 * is collected by the next successfully synchronized Track operation; do not
 * submit the frame again. */
h2_pal_result_t h2_gizclaw_pcm_capture_frame_submit(
    h2_gizclaw_track_t* track, h2_gizclaw_pcm_capture_id_t id,
    const uint8_t* pcm, size_t len, h2_pal_result_t producer_result);

/** Request END. Repeated END on STOPPING/FROZEN/DONE succeeds. With an
 * in-flight producer frame the state is STOPPING until submit returns it;
 * otherwise it is immediately FROZEN. Acceptance is not producer
 * acknowledgement. FAILED returns its latched result; discarded captures return
 * CLOSED. Frozen PCM remains for later adoption, even when END precedes network
 * readiness. */
h2_pal_result_t h2_gizclaw_pcm_capture_end(h2_gizclaw_track_t* track,
                                           h2_gizclaw_pcm_capture_id_t id);

/** Request DISCARD, dropping pending PCM and rejecting further acquisition.
 * Repeated DISCARD succeeds. DISCARDING becomes DISCARDED only after the leased
 * producer frame returns; a managed reader must also deinit before reuse. DONE
 * is already complete and stays DONE; a FAILED result remains latched in the
 * snapshot even if its data is discarded. */
h2_pal_result_t h2_gizclaw_pcm_capture_discard(h2_gizclaw_track_t* track,
                                               h2_gizclaw_pcm_capture_id_t id);

/** Copy a coherent snapshot to required out_snapshot. ID zero selects current
 * (IDLE with id zero before first begin). adopted records whether a reader was
 * ever bound; consumer_busy lasts through reader deinit, including after EOS.
 * captured_bytes counts accepted PCM including consumed bytes; pending_bytes
 * counts unread PCM. result is the first capture failure, otherwise OK. */
h2_pal_result_t h2_gizclaw_pcm_capture_snapshot(
    h2_gizclaw_track_t* track, h2_gizclaw_pcm_capture_id_t id,
    h2_gizclaw_pcm_capture_snapshot_t* out_snapshot);

/** Application/mic producer: append to uplink. One producer only.
 * Returns BUSY while a managed capture occupies the ring.
 * All-or-nothing, nonblocking: WOULD_BLOCK leaves bytes and cursors unchanged.
 * Length must be nonzero, sample aligned, and no larger than capacity. */
h2_pal_result_t h2_gizclaw_pcm_track_write(h2_gizclaw_track_t* track,
                                           const uint8_t* pcm, size_t len);

/** Application/speaker consumer: read exactly len bytes from downlink.
 * One consumer only. WOULD_BLOCK does not consume a partial frame or change
 * the output buffer. Length obeys the same alignment/capacity constraints. */
h2_pal_result_t h2_gizclaw_pcm_track_read(h2_gizclaw_track_t* track,
                                          uint8_t* pcm, size_t len);

#ifdef __cplusplus
}
#endif

#endif
