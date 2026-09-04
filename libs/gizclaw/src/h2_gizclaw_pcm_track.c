#include "h2_gizclaw_pcm_ring.h"
#include "h2_gizclaw_pcm_track_internal.h"


#include <string.h>

typedef struct pcm_track {
  h2_gizclaw_track_t base;
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_pcm_ring_t uplink;
  h2_gizclaw_pcm_ring_t downlink;
  atomic_bool bound;
} pcm_track_t;

static h2_pal_result_t read_uplink(void *user, uint8_t *pcm, size_t len,
                                   size_t *out_len) {
  pcm_track_t *track = user;
  if (out_len == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_len = 0u;
  if ((len & 1u) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = h2_gizclaw_pcm_ring_read(&track->uplink, pcm, len);
  if (rc == H2_PAL_OK)
    *out_len = len;
  return rc;
}

static h2_pal_result_t write_downlink(void *user, const uint8_t *pcm,
                                      size_t len) {
  pcm_track_t *track = user;
  if ((len & 1u) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_pcm_ring_write(&track->downlink, pcm, len);
}

static const h2_gizclaw_track_vtable_t pcm_vtable = {.read = read_uplink,
                                                     .write = write_downlink};

static pcm_track_t *owned_track(h2_gizclaw_track_t *track) {
  return track != NULL && track->vtable == &pcm_vtable ? track->user : NULL;
}

static bool valid_capacity(size_t capacity) {
  /* Power-of-two capacity also keeps byte offsets correct when the unsigned
   * monotonically increasing counters wrap around SIZE_MAX. */
  return capacity >= 2u && capacity <= SIZE_MAX / 2u &&
         (capacity & (capacity - 1u)) == 0u;
}

h2_pal_result_t
h2_gizclaw_pcm_track_create(const h2_gizclaw_pcm_track_config_t *config,
                            h2_gizclaw_track_t **out_track) {
  if (out_track != NULL)
    *out_track = NULL;
  if (config == NULL || config->allocator == NULL ||
      config->allocator->vtable == NULL ||
      config->allocator->vtable->alloc == NULL ||
      config->allocator->vtable->free == NULL || out_track == NULL ||
      (config->uplink_capacity != 0u && !valid_capacity(config->uplink_capacity)) ||
      (config->downlink_capacity != 0u && !valid_capacity(config->downlink_capacity)))
    return H2_PAL_ERR_INVALID_ARG;
  pcm_track_t *track = h2_pal_mem_alloc(config->allocator, sizeof(*track));
  if (track == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(track, 0, sizeof(*track));
  track->allocator = config->allocator;
  track->base = (h2_gizclaw_track_t){.user = track, .vtable = &pcm_vtable};
  atomic_init(&track->bound, false);
  h2_pal_result_t rc = h2_gizclaw_pcm_ring_init(
      &track->uplink, config->allocator, config->uplink_capacity != 0u
          ? config->uplink_capacity : H2_GIZCLAW_PCM_TRACK_DEFAULT_CAPACITY);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_pcm_ring_init(&track->downlink, config->allocator,
        config->downlink_capacity != 0u ? config->downlink_capacity
                                       : H2_GIZCLAW_PCM_TRACK_DEFAULT_CAPACITY);
  if (rc == H2_PAL_OK && (!atomic_is_lock_free(&track->uplink.write_index) ||
                          !atomic_is_lock_free(&track->uplink.read_index) ||
                          !atomic_is_lock_free(&track->downlink.write_index) ||
                          !atomic_is_lock_free(&track->downlink.read_index)))
    rc = H2_PAL_ERR_UNSUPPORTED;
  if (rc != H2_PAL_OK) {
    h2_gizclaw_pcm_ring_deinit(&track->downlink);
    h2_gizclaw_pcm_ring_deinit(&track->uplink);
    h2_pal_mem_free(config->allocator, track);
    return rc;
  }
  *out_track = &track->base;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_pcm_track_destroy(h2_gizclaw_track_t **base) {
  if (base == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (*base == NULL)
    return H2_PAL_OK;
  pcm_track_t *track = owned_track(*base);
  if (track == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (atomic_load_explicit(&track->bound, memory_order_acquire))
    return H2_PAL_ERR_BUSY;
  const h2_pal_mem_api_t *allocator = track->allocator;
  h2_gizclaw_pcm_ring_deinit(&track->uplink);
  h2_gizclaw_pcm_ring_deinit(&track->downlink);
  h2_pal_mem_free(allocator, track);
  *base = NULL;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_pcm_track_write(h2_gizclaw_track_t *base,
                                           const uint8_t *pcm, size_t len) {
  pcm_track_t *track = owned_track(base);
  if (track == NULL || (len & 1u) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_pcm_ring_write(&track->uplink, pcm, len);
}

h2_pal_result_t h2_gizclaw_pcm_track_read(h2_gizclaw_track_t *base,
                                          uint8_t *pcm, size_t len) {
  pcm_track_t *track = owned_track(base);
  if (track == NULL || (len & 1u) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  return h2_gizclaw_pcm_ring_read(&track->downlink, pcm, len);
}

h2_pal_result_t h2_gizclaw_pcm_track_attach_internal(h2_gizclaw_track_t *base) {
  pcm_track_t *track = owned_track(base);
  if (track == NULL)
    return H2_PAL_OK;
  bool expected = false;
  return atomic_compare_exchange_strong_explicit(&track->bound, &expected, true,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)
             ? H2_PAL_OK
             : H2_PAL_ERR_BUSY;
}

void h2_gizclaw_pcm_track_detach_internal(h2_gizclaw_track_t *base) {
  pcm_track_t *track = owned_track(base);
  if (track != NULL)
    atomic_store_explicit(&track->bound, false, memory_order_release);
}

size_t h2_gizclaw_pcm_track_pending_internal(h2_gizclaw_track_t *base) {
  pcm_track_t *track = owned_track(base);
  return track == NULL ? 0u : h2_gizclaw_pcm_ring_available(&track->uplink);
}

bool h2_gizclaw_pcm_track_downlink_stats_internal(h2_gizclaw_track_t *base,
                                                  size_t *out_used,
                                                  size_t *out_capacity) {
  pcm_track_t *track = owned_track(base);
  if (track == NULL || out_used == NULL || out_capacity == NULL)
    return false;
  *out_used = h2_gizclaw_pcm_ring_available(&track->downlink);
  *out_capacity = track->downlink.capacity;
  if (*out_used > *out_capacity)
    *out_used = 0u;
  return true;
}

h2_pal_result_t h2_gizclaw_pcm_input_start(h2_gizclaw_pcm_input_t *input,
                                          h2_gizclaw_track_t *base,
                                          const h2_pal_mem_api_t *allocator) {
  if (input == NULL || base == NULL || allocator == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (input->active || input->ended)
    return H2_PAL_ERR_INVALID_STATE;
  pcm_track_t *track = owned_track(base);
  input->track = base;
  input->tail_allocator = allocator;
  input->begin = track == NULL ? 0u : atomic_load_explicit(
      &track->uplink.write_index, memory_order_acquire);
  input->active = true;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_pcm_input_end(h2_gizclaw_pcm_input_t *input,
                                        h2_gizclaw_track_t *base) {
  if (input == NULL || base == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (input->ended)
    return H2_PAL_OK;
  if (!input->active || input->track != base)
    return H2_PAL_ERR_INVALID_STATE;
  pcm_track_t *track = owned_track(base);
  input->end = track == NULL ? 0u : atomic_load_explicit(
      &track->uplink.write_index, memory_order_acquire);
  input->ended = true;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_pcm_input_prepare(h2_gizclaw_pcm_input_t *input,
                                            h2_gizclaw_track_t *base) {
  if (input == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (!input->active)
    return H2_PAL_ERR_WOULD_BLOCK;
  if (base == NULL || input->track != base)
    return H2_PAL_ERR_CLOSED;
  pcm_track_t *track = owned_track(base);
  if (track == NULL) { /* Private fault-injection port, not an app vtable. */
    input->begun = true;
    input->tail_taken = input->ended;
    return H2_PAL_OK;
  }
  h2_gizclaw_pcm_ring_t *ring = &track->uplink;
  size_t read = atomic_load_explicit(&ring->read_index, memory_order_relaxed);
  if (!input->begun) {
    if (input->begin - read > ring->capacity)
      return H2_PAL_ERR_INVALID_STATE;
    atomic_store_explicit(&ring->read_index, input->begin, memory_order_release);
    read = input->begin;
    input->begun = true;
  }
  if (input->ended && !input->tail_taken) {
    const size_t len = input->end - read;
    if (len > ring->capacity)
      return H2_PAL_ERR_INVALID_STATE;
    if (len != 0u) {
      uint8_t *tail = h2_pal_mem_alloc(input->tail_allocator, len);
      if (tail == NULL)
        return H2_PAL_ERR_NO_MEMORY;
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

h2_pal_result_t h2_gizclaw_pcm_input_read(h2_gizclaw_pcm_input_t *input,
    h2_gizclaw_track_t *base, uint8_t *pcm, size_t capacity, size_t *out_len) {
  if (out_len != NULL)
    *out_len = 0u;
  if (input == NULL || pcm == NULL || capacity == 0u || (capacity & 1u) != 0u || out_len == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = h2_gizclaw_pcm_input_prepare(input, base);
  if (rc != H2_PAL_OK)
    return rc;
  if (!input->ended) {
    rc = base->vtable->read == NULL ? H2_PAL_ERR_UNSUPPORTED
        : base->vtable->read(base->user, pcm, capacity, out_len);
    if (rc == H2_PAL_OK && (*out_len == 0u || *out_len > capacity ||
                            *out_len % sizeof(int16_t) != 0u))
      rc = H2_PAL_ERR_FORMAT;
    if (rc != H2_PAL_OK)
      *out_len = 0u;
    return rc;
  }
  size_t len = input->tail_len - input->tail_offset;
  if (len > capacity)
    len = capacity;
  if (len != 0u)
    memcpy(pcm, input->tail + input->tail_offset, len);
  input->tail_offset += len;
  *out_len = len;
  return H2_PAL_OK;
}

void h2_gizclaw_pcm_input_deinit(h2_gizclaw_pcm_input_t *input) {
  if (input->tail != NULL)
    h2_pal_mem_free(input->tail_allocator, input->tail);
  memset(input, 0, sizeof(*input));
}
