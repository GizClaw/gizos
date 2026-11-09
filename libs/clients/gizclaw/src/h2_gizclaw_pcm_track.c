#include "h2_gizclaw_pcm_track_internal.h"

#include "h2_gizclaw_pcm_ring.h"

#include <string.h>

typedef struct pcm_track {
  h2_gizclaw_track_t base;
  const h2_pal_mem_api_t* allocator;
  const h2_pal_sync_api_t* sync;
  h2_pal_mutex_t* mutex;
  h2_gizclaw_pcm_ring_t uplink;
  h2_gizclaw_pcm_ring_t downlink;
  h2_gizclaw_pcm_capture_snapshot_t capture;
  size_t capture_begin;
  h2_atomic_bool_t bound;
  /* A new audio request records the downlink write index here; the consumer
   * skips everything before it on its next read. Only the consumer moves
   * the ring's read index, so the SPSC contract holds. */
  h2_atomic_size_t downlink_discard_until;
  h2_atomic_bool_t downlink_discard_pending;
} pcm_track_t;

static h2_pal_result_t track_lock(pcm_track_t* track) {
  return track->sync == NULL ? H2_PAL_OK
                             : h2_pal_mutex_lock(track->sync, track->mutex);
}

static h2_pal_result_t track_unlock(pcm_track_t* track,
                                    h2_pal_result_t result) {
  if (track->sync == NULL) return result;
  h2_pal_result_t rc = h2_pal_mutex_unlock(track->sync, track->mutex);
  return result == H2_PAL_OK ? rc : result;
}

/* The caller holds the Track mutex when sync is configured. */
static bool capture_busy(const pcm_track_t* track) {
  return track->capture.producer_busy || track->capture.consumer_busy ||
         track->capture.state == H2_GIZCLAW_PCM_CAPTURE_CAPTURING ||
         track->capture.state == H2_GIZCLAW_PCM_CAPTURE_STOPPING ||
         track->capture.state == H2_GIZCLAW_PCM_CAPTURE_FROZEN ||
         track->capture.state == H2_GIZCLAW_PCM_CAPTURE_DISCARDING;
}

static void discard_uplink(pcm_track_t* track) {
  h2_atomic_store_explicit(
      &track->uplink.read_index,
      h2_atomic_load_explicit(&track->uplink.write_index, H2_ATOMIC_ACQUIRE),
      H2_ATOMIC_RELEASE);
  track->capture.pending_bytes = 0u;
}

static h2_pal_result_t read_uplink(void* user, uint8_t* pcm, size_t len,
                                   size_t* out_len) {
  pcm_track_t* track = user;
  if (out_len == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_len = 0u;
  if ((len & 1u) != 0u) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = track_lock(track);
  if (rc != H2_PAL_OK) return rc;
  rc = capture_busy(track) ? H2_PAL_ERR_WOULD_BLOCK
                           : h2_gizclaw_pcm_ring_read(&track->uplink, pcm, len);
  if (rc == H2_PAL_OK) *out_len = len;
  return track_unlock(track, rc);
}

static h2_pal_result_t write_downlink(void* user, const uint8_t* pcm,
                                      size_t len) {
  pcm_track_t* track = user;
  if ((len & 1u) != 0u) return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_pcm_ring_write(&track->downlink, pcm, len);
}

static const h2_gizclaw_track_vtable_t pcm_vtable = {.read = read_uplink,
                                                     .write = write_downlink};

static pcm_track_t* owned_track(h2_gizclaw_track_t* track) {
  return track != NULL && track->vtable == &pcm_vtable ? track->user : NULL;
}

static h2_pal_result_t capture_lock(h2_gizclaw_track_t* base,
                                    h2_gizclaw_pcm_capture_id_t id,
                                    pcm_track_t** out_track) {
  pcm_track_t* track = owned_track(base);
  if (track == NULL || id == 0u) return H2_PAL_ERR_INVALID_ARG;
  if (track->sync == NULL) return H2_PAL_ERR_UNSUPPORTED;
  h2_pal_result_t rc = track_lock(track);
  if (rc != H2_PAL_OK) return rc;
  if (track->capture.id != id) return track_unlock(track, H2_PAL_ERR_NOT_FOUND);
  *out_track = track;
  return H2_PAL_OK;
}

static bool valid_capacity(size_t capacity) {
  /* Power-of-two capacity also keeps byte offsets correct when the unsigned
   * monotonically increasing counters wrap around SIZE_MAX. */
  return capacity >= 2u && capacity <= SIZE_MAX / 2u &&
         (capacity & (capacity - 1u)) == 0u;
}

h2_pal_result_t h2_gizclaw_pcm_track_create(
    const h2_gizclaw_pcm_track_config_t* config,
    h2_gizclaw_track_t** out_track) {
  if (out_track != NULL) *out_track = NULL;
  if (config == NULL || config->allocator == NULL ||
      config->allocator->vtable == NULL ||
      config->allocator->vtable->alloc == NULL ||
      config->allocator->vtable->free == NULL || out_track == NULL ||
      (config->uplink_capacity != 0u &&
       !valid_capacity(config->uplink_capacity)) ||
      (config->downlink_capacity != 0u &&
       !valid_capacity(config->downlink_capacity)))
    return H2_PAL_ERR_INVALID_ARG;
  if (config->sync != NULL && (config->sync->vtable == NULL ||
                               config->sync->vtable->create_mutex == NULL ||
                               config->sync->vtable->destroy_mutex == NULL ||
                               config->sync->vtable->lock_mutex == NULL ||
                               config->sync->vtable->unlock_mutex == NULL))
    return H2_PAL_ERR_UNSUPPORTED;
  pcm_track_t* track = h2_pal_mem_alloc(config->allocator, sizeof(*track));
  if (track == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(track, 0, sizeof(*track));
  track->allocator = config->allocator;
  track->sync = config->sync;
  track->base = (h2_gizclaw_track_t){.user = track, .vtable = &pcm_vtable};
  h2_pal_result_t rc = H2_PAL_OK;
  if (h2_atomic_bool_init(&track->bound, false) != H2_ATOMIC_OK ||
      h2_atomic_size_init(&track->downlink_discard_until, 0u) != H2_ATOMIC_OK ||
      h2_atomic_bool_init(&track->downlink_discard_pending, false) !=
          H2_ATOMIC_OK)
    rc = H2_PAL_ERR_NO_MEMORY;
  if (rc == H2_PAL_OK && track->sync != NULL) {
    const h2_pal_mutex_config_t mutex_config = {.name = "pcm_capture",
                                                .allocator = track->allocator};
    rc = h2_pal_mutex_create(track->sync, &mutex_config, &track->mutex);
    if (rc == H2_PAL_OK && track->mutex == NULL) rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_pcm_ring_init(&track->uplink, config->allocator,
                                  config->uplink_capacity != 0u
                                      ? config->uplink_capacity
                                      : H2_GIZCLAW_PCM_TRACK_DEFAULT_CAPACITY);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_pcm_ring_init(&track->downlink, config->allocator,
                                  config->downlink_capacity != 0u
                                      ? config->downlink_capacity
                                      : H2_GIZCLAW_PCM_TRACK_DEFAULT_CAPACITY);
  if (rc != H2_PAL_OK) {
    h2_gizclaw_pcm_ring_deinit(&track->downlink);
    h2_gizclaw_pcm_ring_deinit(&track->uplink);
    if (track->mutex != NULL)
      (void)h2_pal_mutex_destroy(track->sync, track->mutex);
    h2_atomic_bool_destroy(&track->bound);
    h2_atomic_size_destroy(&track->downlink_discard_until);
    h2_atomic_bool_destroy(&track->downlink_discard_pending);
    h2_pal_mem_free(config->allocator, track);
    return rc;
  }
  *out_track = &track->base;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_pcm_track_destroy(h2_gizclaw_track_t** base) {
  if (base == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (*base == NULL) return H2_PAL_OK;
  pcm_track_t* track = owned_track(*base);
  if (track == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = track_lock(track);
  if (rc != H2_PAL_OK) return rc;
  const bool busy = capture_busy(track) ||
                    h2_atomic_load_explicit(&track->bound, H2_ATOMIC_ACQUIRE);
  rc = track_unlock(track, busy ? H2_PAL_ERR_BUSY : H2_PAL_OK);
  if (rc != H2_PAL_OK) return rc;
  if (track->mutex != NULL) {
    rc = h2_pal_mutex_destroy(track->sync, track->mutex);
    if (rc != H2_PAL_OK) return rc;
  }
  const h2_pal_mem_api_t* allocator = track->allocator;
  h2_gizclaw_pcm_ring_deinit(&track->uplink);
  h2_gizclaw_pcm_ring_deinit(&track->downlink);
  h2_atomic_bool_destroy(&track->bound);
  h2_atomic_size_destroy(&track->downlink_discard_until);
  h2_atomic_bool_destroy(&track->downlink_discard_pending);
  h2_pal_mem_free(allocator, track);
  *base = NULL;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_pcm_capture_begin(
    h2_gizclaw_track_t* base, h2_gizclaw_pcm_capture_id_t* out_id) {
  if (out_id != NULL) *out_id = 0u;
  pcm_track_t* track = owned_track(base);
  if (track == NULL || out_id == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (track->sync == NULL) return H2_PAL_ERR_UNSUPPORTED;
  h2_pal_result_t rc = track_lock(track);
  if (rc != H2_PAL_OK) return rc;
  if (capture_busy(track)) return track_unlock(track, H2_PAL_ERR_BUSY);
  /* Never wrap an identity into one that could still exist in a stale caller.
   */
  if (track->capture.id == UINT64_MAX)
    return track_unlock(track, H2_PAL_ERR_NO_SPACE);
  const h2_gizclaw_pcm_capture_id_t id = track->capture.id + 1u;
  discard_uplink(track);
  track->capture_begin =
      h2_atomic_load_explicit(&track->uplink.write_index, H2_ATOMIC_ACQUIRE);
  track->capture = (h2_gizclaw_pcm_capture_snapshot_t){
      .id = id, .state = H2_GIZCLAW_PCM_CAPTURE_CAPTURING, .result = H2_PAL_OK};
  *out_id = id;
  return track_unlock(track, H2_PAL_OK);
}

h2_pal_result_t h2_gizclaw_pcm_capture_frame_acquire(
    h2_gizclaw_track_t* base, h2_gizclaw_pcm_capture_id_t id) {
  pcm_track_t* track;
  h2_pal_result_t rc = capture_lock(base, id, &track);
  if (rc != H2_PAL_OK) return rc;
  if (track->capture.state == H2_GIZCLAW_PCM_CAPTURE_FAILED)
    rc = track->capture.result;
  else if (track->capture.state != H2_GIZCLAW_PCM_CAPTURE_CAPTURING)
    rc = H2_PAL_ERR_CLOSED;
  else if (track->capture.producer_busy)
    rc = H2_PAL_ERR_BUSY;
  else
    track->capture.producer_busy = true;
  return track_unlock(track, rc);
}

h2_pal_result_t h2_gizclaw_pcm_capture_frame_submit(
    h2_gizclaw_track_t* base, h2_gizclaw_pcm_capture_id_t id,
    const uint8_t* pcm, size_t len, h2_pal_result_t producer_result) {
  pcm_track_t* track;
  h2_pal_result_t rc = capture_lock(base, id, &track);
  if (rc != H2_PAL_OK) return rc;
  if (!track->capture.producer_busy)
    return track_unlock(track, H2_PAL_ERR_INVALID_STATE);
  track->capture.producer_busy = false;
  if (track->capture.state == H2_GIZCLAW_PCM_CAPTURE_DISCARDING) {
    track->capture.state = H2_GIZCLAW_PCM_CAPTURE_DISCARDED;
    return track_unlock(track, H2_PAL_OK);
  }
  const bool empty = pcm == NULL && len == 0u &&
                     (producer_result == H2_PAL_ERR_WOULD_BLOCK ||
                      producer_result == H2_PAL_ERR_TIMEOUT);
  if (!empty) {
    rc = producer_result;
    if (rc == H2_PAL_OK) {
      if (pcm == NULL || len == 0u || (len & 1u) != 0u)
        rc = H2_PAL_ERR_INVALID_ARG;
      else if (len > track->uplink.capacity ||
               track->capture.captured_bytes > SIZE_MAX - len)
        rc = H2_PAL_ERR_NO_SPACE;
      else {
        rc = h2_gizclaw_pcm_ring_write(&track->uplink, pcm, len);
        if (rc == H2_PAL_ERR_WOULD_BLOCK) rc = H2_PAL_ERR_NO_SPACE;
        if (rc == H2_PAL_OK) {
          track->capture.captured_bytes += len;
          track->capture.pending_bytes += len;
        }
      }
    }
  }
  if (rc != H2_PAL_OK) {
    if (track->capture.result == H2_PAL_OK) track->capture.result = rc;
    track->capture.state = H2_GIZCLAW_PCM_CAPTURE_FAILED;
  } else if (track->capture.state == H2_GIZCLAW_PCM_CAPTURE_STOPPING) {
    track->capture.state = H2_GIZCLAW_PCM_CAPTURE_FROZEN;
  }
  return track_unlock(track, rc);
}

h2_pal_result_t h2_gizclaw_pcm_capture_end(h2_gizclaw_track_t* base,
                                           h2_gizclaw_pcm_capture_id_t id) {
  pcm_track_t* track;
  h2_pal_result_t rc = capture_lock(base, id, &track);
  if (rc != H2_PAL_OK) return rc;
  switch (track->capture.state) {
    case H2_GIZCLAW_PCM_CAPTURE_CAPTURING:
      track->capture.state = track->capture.producer_busy
                                 ? H2_GIZCLAW_PCM_CAPTURE_STOPPING
                                 : H2_GIZCLAW_PCM_CAPTURE_FROZEN;
      break;
    case H2_GIZCLAW_PCM_CAPTURE_STOPPING:
    case H2_GIZCLAW_PCM_CAPTURE_FROZEN:
    case H2_GIZCLAW_PCM_CAPTURE_DONE:
      break;
    case H2_GIZCLAW_PCM_CAPTURE_FAILED:
      rc = track->capture.result;
      break;
    default:
      rc = H2_PAL_ERR_CLOSED;
      break;
  }
  return track_unlock(track, rc);
}

h2_pal_result_t h2_gizclaw_pcm_capture_discard(h2_gizclaw_track_t* base,
                                               h2_gizclaw_pcm_capture_id_t id) {
  pcm_track_t* track;
  h2_pal_result_t rc = capture_lock(base, id, &track);
  if (rc != H2_PAL_OK) return rc;
  if (track->capture.state != H2_GIZCLAW_PCM_CAPTURE_DONE &&
      track->capture.state != H2_GIZCLAW_PCM_CAPTURE_DISCARDED) {
    discard_uplink(track);
    track->capture.state = track->capture.producer_busy
                               ? H2_GIZCLAW_PCM_CAPTURE_DISCARDING
                               : H2_GIZCLAW_PCM_CAPTURE_DISCARDED;
  }
  return track_unlock(track, H2_PAL_OK);
}

h2_pal_result_t h2_gizclaw_pcm_capture_snapshot(
    h2_gizclaw_track_t* base, h2_gizclaw_pcm_capture_id_t id,
    h2_gizclaw_pcm_capture_snapshot_t* out_snapshot) {
  pcm_track_t* track = owned_track(base);
  if (track == NULL || out_snapshot == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (track->sync == NULL) return H2_PAL_ERR_UNSUPPORTED;
  h2_pal_result_t rc = track_lock(track);
  if (rc != H2_PAL_OK) return rc;
  if (id != 0u && track->capture.id != id)
    return track_unlock(track, H2_PAL_ERR_NOT_FOUND);
  *out_snapshot = track->capture;
  return track_unlock(track, H2_PAL_OK);
}

bool h2_gizclaw_pcm_capture_busy_internal(h2_gizclaw_track_t* base) {
  pcm_track_t* track = owned_track(base);
  if (track == NULL || track->sync == NULL) return false;
  if (track_lock(track) != H2_PAL_OK) return true;
  const bool busy = capture_busy(track);
  return track_unlock(track, H2_PAL_OK) != H2_PAL_OK || busy;
}

h2_pal_result_t h2_gizclaw_pcm_track_write(h2_gizclaw_track_t* base,
                                           const uint8_t* pcm, size_t len) {
  pcm_track_t* track = owned_track(base);
  if (track == NULL || (len & 1u) != 0u) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = track_lock(track);
  if (rc != H2_PAL_OK) return rc;
  rc = capture_busy(track)
           ? H2_PAL_ERR_BUSY
           : h2_gizclaw_pcm_ring_write(&track->uplink, pcm, len);
  return track_unlock(track, rc);
}

h2_pal_result_t h2_gizclaw_pcm_track_read(h2_gizclaw_track_t* base,
                                          uint8_t* pcm, size_t len) {
  pcm_track_t* track = owned_track(base);
  if (track == NULL || (len & 1u) != 0u) return H2_PAL_ERR_INVALID_ARG;
  if (h2_atomic_exchange_explicit(&track->downlink_discard_pending, false,
                                  H2_ATOMIC_ACQ_REL)) {
    /* Stale playback from the previous request ends here; the writer may
     * already have appended the new request's audio past the watermark. */
    h2_gizclaw_pcm_ring_t* ring = &track->downlink;
    const size_t until = h2_atomic_load_explicit(&track->downlink_discard_until,
                                                 H2_ATOMIC_ACQUIRE);
    const size_t read =
        h2_atomic_load_explicit(&ring->read_index, H2_ATOMIC_RELAXED);
    if (until - read <= ring->capacity)
      h2_atomic_store_explicit(&ring->read_index, until, H2_ATOMIC_RELEASE);
  }
  return h2_gizclaw_pcm_ring_read(&track->downlink, pcm, len);
}

void h2_gizclaw_pcm_track_discard_downlink_internal(h2_gizclaw_track_t* base) {
  pcm_track_t* track = owned_track(base);
  if (track == NULL) return;
  h2_atomic_store_explicit(
      &track->downlink_discard_until,
      h2_atomic_load_explicit(&track->downlink.write_index, H2_ATOMIC_ACQUIRE),
      H2_ATOMIC_RELEASE);
  h2_atomic_store_explicit(&track->downlink_discard_pending, true,
                           H2_ATOMIC_RELEASE);
}

h2_pal_result_t h2_gizclaw_pcm_track_attach_internal(h2_gizclaw_track_t* base) {
  pcm_track_t* track = owned_track(base);
  if (track == NULL) return H2_PAL_OK;
  bool expected = false;
  return h2_atomic_compare_exchange_strong_explicit(&track->bound, &expected,
                                                    true, H2_ATOMIC_ACQ_REL,
                                                    H2_ATOMIC_ACQUIRE)
             ? H2_PAL_OK
             : H2_PAL_ERR_BUSY;
}

void h2_gizclaw_pcm_track_detach_internal(h2_gizclaw_track_t* base) {
  pcm_track_t* track = owned_track(base);
  if (track != NULL)
    h2_atomic_store_explicit(&track->bound, false, H2_ATOMIC_RELEASE);
}

size_t h2_gizclaw_pcm_track_pending_internal(h2_gizclaw_track_t* base) {
  pcm_track_t* track = owned_track(base);
  if (track == NULL || track_lock(track) != H2_PAL_OK) return 0u;
  const size_t len = h2_gizclaw_pcm_ring_available(&track->uplink);
  (void)track_unlock(track, H2_PAL_OK);
  return len;
}

h2_pal_result_t h2_gizclaw_pcm_input_adopt(h2_gizclaw_pcm_input_t* input,
                                           h2_gizclaw_track_t* base,
                                           const h2_pal_mem_api_t* allocator,
                                           h2_gizclaw_pcm_capture_id_t id) {
  if (input == NULL || allocator == NULL) return H2_PAL_ERR_INVALID_ARG;
  pcm_track_t* track;
  h2_pal_result_t rc = capture_lock(base, id, &track);
  if (rc != H2_PAL_OK) return rc;
  if (input->capture_id == id && input->track == base)
    return track_unlock(track, H2_PAL_OK);
  if (input->capture_id != 0u || input->begun || input->ended ||
      input->tail != NULL || (input->active && input->track != base))
    return track_unlock(track, H2_PAL_ERR_INVALID_STATE);
  if (track->capture.consumer_busy || track->capture.adopted)
    return track_unlock(track, H2_PAL_ERR_BUSY);
  if (track->capture.state == H2_GIZCLAW_PCM_CAPTURE_FAILED)
    return track_unlock(track, track->capture.result);
  if (track->capture.state != H2_GIZCLAW_PCM_CAPTURE_CAPTURING &&
      track->capture.state != H2_GIZCLAW_PCM_CAPTURE_STOPPING &&
      track->capture.state != H2_GIZCLAW_PCM_CAPTURE_FROZEN)
    return track_unlock(track, H2_PAL_ERR_CLOSED);
  *input = (h2_gizclaw_pcm_input_t){.track = base,
                                    .begin = track->capture_begin,
                                    .tail_allocator = allocator,
                                    .active = true,
                                    .capture_id = id};
  track->capture.adopted = true;
  track->capture.consumer_busy = true;
  return track_unlock(track, H2_PAL_OK);
}

h2_pal_result_t h2_gizclaw_pcm_input_start(h2_gizclaw_pcm_input_t* input,
                                           h2_gizclaw_track_t* base,
                                           const h2_pal_mem_api_t* allocator) {
  if (input == NULL || base == NULL || allocator == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (input->active || input->ended) return H2_PAL_ERR_INVALID_STATE;
  pcm_track_t* track = owned_track(base);
  if (track != NULL) {
    h2_pal_result_t rc = track_lock(track);
    if (rc != H2_PAL_OK) return rc;
    if (capture_busy(track)) return track_unlock(track, H2_PAL_ERR_WOULD_BLOCK);
  }
  input->track = base;
  input->tail_allocator = allocator;
  input->begin = track == NULL
                     ? 0u
                     : h2_atomic_load_explicit(&track->uplink.write_index,
                                               H2_ATOMIC_ACQUIRE);
  input->active = true;
  return track == NULL ? H2_PAL_OK : track_unlock(track, H2_PAL_OK);
}

h2_pal_result_t h2_gizclaw_pcm_input_end(h2_gizclaw_pcm_input_t* input,
                                         h2_gizclaw_track_t* base) {
  if (input == NULL || base == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (input->capture_id != 0u) {
    if (input->track != base) return H2_PAL_ERR_INVALID_STATE;
    return h2_gizclaw_pcm_capture_end(base, input->capture_id);
  }
  if (input->ended) return H2_PAL_OK;
  if (!input->active || input->track != base) return H2_PAL_ERR_INVALID_STATE;
  pcm_track_t* track = owned_track(base);
  if (track != NULL) {
    h2_pal_result_t rc = track_lock(track);
    if (rc != H2_PAL_OK) return rc;
    if (capture_busy(track)) return track_unlock(track, H2_PAL_ERR_WOULD_BLOCK);
  }
  input->end = track == NULL
                   ? 0u
                   : h2_atomic_load_explicit(&track->uplink.write_index,
                                             H2_ATOMIC_ACQUIRE);
  input->empty = track != NULL && input->end == input->begin;
  input->ended = true;
  return track == NULL ? H2_PAL_OK : track_unlock(track, H2_PAL_OK);
}

/* Called with the Track mutex held. Legacy fields still report drain completion
 * to existing uplink routes, but there is no private tail in managed mode. */
static h2_pal_result_t managed_prepare(h2_gizclaw_pcm_input_t* input,
                                       pcm_track_t* track) {
  if (track->capture.id != input->capture_id) return H2_PAL_ERR_NOT_FOUND;
  if (!track->capture.consumer_busy) return H2_PAL_ERR_INVALID_STATE;
  if (track->capture.state == H2_GIZCLAW_PCM_CAPTURE_FAILED)
    return track->capture.result;
  if (track->capture.state == H2_GIZCLAW_PCM_CAPTURE_DISCARDING ||
      track->capture.state == H2_GIZCLAW_PCM_CAPTURE_DISCARDED)
    return H2_PAL_ERR_CLOSED;
  input->begun = true;
  if (track->capture.state == H2_GIZCLAW_PCM_CAPTURE_FROZEN ||
      track->capture.state == H2_GIZCLAW_PCM_CAPTURE_DONE) {
    input->ended = true;
    input->empty = track->capture.captured_bytes == 0u;
    input->end =
        h2_atomic_load_explicit(&track->uplink.write_index, H2_ATOMIC_ACQUIRE);
    if (h2_gizclaw_pcm_ring_available(&track->uplink) == 0u) {
      input->tail_taken = true;
      track->capture.state = H2_GIZCLAW_PCM_CAPTURE_DONE;
    }
  }
  return H2_PAL_OK;
}

static h2_pal_result_t legacy_prepare(h2_gizclaw_pcm_input_t* input,
                                      pcm_track_t* track) {
  if (capture_busy(track)) return H2_PAL_ERR_WOULD_BLOCK;
  h2_gizclaw_pcm_ring_t* ring = &track->uplink;
  size_t read = h2_atomic_load_explicit(&ring->read_index, H2_ATOMIC_RELAXED);
  if (!input->begun) {
    if (input->begin - read > ring->capacity) return H2_PAL_ERR_INVALID_STATE;
    h2_atomic_store_explicit(&ring->read_index, input->begin,
                             H2_ATOMIC_RELEASE);
    read = input->begin;
    input->begun = true;
  }
  if (input->ended && !input->tail_taken) {
    const size_t len = input->end - read;
    if (len > ring->capacity) return H2_PAL_ERR_INVALID_STATE;
    if (len != 0u) {
      uint8_t* tail = h2_pal_mem_alloc(input->tail_allocator, len);
      if (tail == NULL) return H2_PAL_ERR_NO_MEMORY;
      /* read copies the entire frozen prefix before publishing read_index.
       * The mic writer cannot reuse those bytes until the copy is complete. */
      h2_pal_result_t rc = h2_gizclaw_pcm_ring_read(ring, tail, len);
      if (rc != H2_PAL_OK) {
        h2_pal_mem_free(input->tail_allocator, tail);
        return rc;
      }
      input->tail = tail;
      input->tail_len = len;
    }
    input->tail_taken = true;
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_pcm_input_prepare(h2_gizclaw_pcm_input_t* input,
                                             h2_gizclaw_track_t* base) {
  if (input == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!input->active) return H2_PAL_ERR_WOULD_BLOCK;
  if (base == NULL || input->track != base) return H2_PAL_ERR_CLOSED;
  pcm_track_t* track = owned_track(base);
  if (track == NULL) { /* Private fault-injection port, not an app vtable. */
    input->begun = true;
    input->tail_taken = input->ended;
    return H2_PAL_OK;
  }
  h2_pal_result_t rc = track_lock(track);
  if (rc != H2_PAL_OK) return rc;
  rc = input->capture_id != 0u ? managed_prepare(input, track)
                               : legacy_prepare(input, track);
  return track_unlock(track, rc);
}

h2_pal_result_t h2_gizclaw_pcm_input_read(h2_gizclaw_pcm_input_t* input,
                                          h2_gizclaw_track_t* base,
                                          uint8_t* pcm, size_t capacity,
                                          size_t* out_len) {
  if (out_len != NULL) *out_len = 0u;
  if (input == NULL || pcm == NULL || capacity == 0u || (capacity & 1u) != 0u ||
      out_len == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (input->capture_id != 0u) {
    if (!input->active || base == NULL || input->track != base)
      return H2_PAL_ERR_CLOSED;
    pcm_track_t* track;
    h2_pal_result_t rc = capture_lock(base, input->capture_id, &track);
    if (rc != H2_PAL_OK) return rc;
    rc = managed_prepare(input, track);
    if (rc == H2_PAL_OK) {
      size_t len = h2_gizclaw_pcm_ring_available(&track->uplink);
      if (len > capacity) len = capacity;
      if (len != 0u) {
        rc = h2_gizclaw_pcm_ring_read(&track->uplink, pcm, len);
        if (rc == H2_PAL_OK) {
          *out_len = len;
          track->capture.pending_bytes -= len;
          rc = managed_prepare(input, track);
        }
      } else if (!input->ended) {
        rc = H2_PAL_ERR_WOULD_BLOCK;
      }
    }
    return track_unlock(track, rc);
  }
  h2_pal_result_t rc = h2_gizclaw_pcm_input_prepare(input, base);
  if (rc != H2_PAL_OK) return rc;
  if (!input->ended) {
    rc = base->vtable->read == NULL
             ? H2_PAL_ERR_UNSUPPORTED
             : base->vtable->read(base->user, pcm, capacity, out_len);
    if (rc == H2_PAL_OK && (*out_len == 0u || *out_len > capacity ||
                            *out_len % sizeof(int16_t) != 0u))
      rc = H2_PAL_ERR_FORMAT;
    if (rc != H2_PAL_OK) *out_len = 0u;
    return rc;
  }
  size_t len = input->tail_len - input->tail_offset;
  if (len > capacity) len = capacity;
  if (len != 0u) memcpy(pcm, input->tail + input->tail_offset, len);
  input->tail_offset += len;
  *out_len = len;
  return H2_PAL_OK;
}

void h2_gizclaw_pcm_input_deinit(h2_gizclaw_pcm_input_t* input) {
  if (input == NULL) return;
  if (input->capture_id != 0u) {
    pcm_track_t* track;
    if (capture_lock(input->track, input->capture_id, &track) != H2_PAL_OK)
      return; /* Retain the lease/input so cleanup can be retried. */
    track->capture.consumer_busy = false;
    if (track->capture.state == H2_GIZCLAW_PCM_CAPTURE_FROZEN) {
      discard_uplink(track);
      track->capture.state = H2_GIZCLAW_PCM_CAPTURE_DONE;
    } else if (track->capture.state == H2_GIZCLAW_PCM_CAPTURE_CAPTURING ||
               track->capture.state == H2_GIZCLAW_PCM_CAPTURE_STOPPING) {
      /* A detached consumer cannot drain a future tail. Stop this window too.
       */
      discard_uplink(track);
      track->capture.state = track->capture.producer_busy
                                 ? H2_GIZCLAW_PCM_CAPTURE_DISCARDING
                                 : H2_GIZCLAW_PCM_CAPTURE_DISCARDED;
    }
    (void)track_unlock(track, H2_PAL_OK);
  }
  if (input->tail != NULL) h2_pal_mem_free(input->tail_allocator, input->tail);
  memset(input, 0, sizeof(*input));
}
