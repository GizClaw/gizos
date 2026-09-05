#ifndef H2_GIZCLAW_PCM_TRACK_FAKE_H
#define H2_GIZCLAW_PCM_TRACK_FAKE_H

#include "h2_gizclaw_pcm_track.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

typedef struct fake_pcm_ring {
  uint8_t *data;
  size_t capacity;
  size_t read;
  size_t write;
  atomic_size_t used;
} fake_pcm_ring_t;

struct h2_gizclaw_track {
  const h2_pal_mem_api_t *allocator;
  fake_pcm_ring_t uplink;
  fake_pcm_ring_t downlink;
  atomic_bool bound;
};

static h2_pal_result_t fake_pcm_ring_write(fake_pcm_ring_t *ring,
                                           const uint8_t *data, size_t len) {
  const size_t used = atomic_load_explicit(&ring->used, memory_order_acquire);
  if (len > ring->capacity - used)
    return H2_PAL_ERR_WOULD_BLOCK;
  const size_t first = len < ring->capacity - ring->write
                           ? len
                           : ring->capacity - ring->write;
  memcpy(ring->data + ring->write, data, first);
  memcpy(ring->data, data + first, len - first);
  ring->write = (ring->write + len) % ring->capacity;
  atomic_fetch_add_explicit(&ring->used, len, memory_order_release);
  return H2_PAL_OK;
}

static h2_pal_result_t fake_pcm_ring_read(fake_pcm_ring_t *ring, uint8_t *data,
                                          size_t len) {
  const size_t used = atomic_load_explicit(&ring->used, memory_order_acquire);
  if (len > used)
    return H2_PAL_ERR_WOULD_BLOCK;
  const size_t first = len < ring->capacity - ring->read
                           ? len
                           : ring->capacity - ring->read;
  memcpy(data, ring->data + ring->read, first);
  memcpy(data + first, ring->data, len - first);
  ring->read = (ring->read + len) % ring->capacity;
  atomic_fetch_sub_explicit(&ring->used, len, memory_order_release);
  return H2_PAL_OK;
}

h2_pal_result_t
h2_gizclaw_pcm_track_create(const h2_gizclaw_pcm_track_config_t *config,
                            h2_gizclaw_track_t **out_track) {
  if (out_track != NULL)
    *out_track = NULL;
  if (config == NULL || config->allocator == NULL || out_track == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const size_t uplink = config->uplink_capacity == 0u
                            ? H2_GIZCLAW_PCM_TRACK_DEFAULT_CAPACITY
                            : config->uplink_capacity;
  const size_t downlink = config->downlink_capacity == 0u
                              ? H2_GIZCLAW_PCM_TRACK_DEFAULT_CAPACITY
                              : config->downlink_capacity;
  h2_gizclaw_track_t *track = h2_pal_mem_alloc(config->allocator, sizeof(*track));
  if (track == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(track, 0, sizeof(*track));
  track->allocator = config->allocator;
  atomic_init(&track->bound, false);
  atomic_init(&track->uplink.used, 0u);
  atomic_init(&track->downlink.used, 0u);
  track->uplink.data = h2_pal_mem_alloc(config->allocator, uplink);
  track->downlink.data = h2_pal_mem_alloc(config->allocator, downlink);
  if (track->uplink.data == NULL || track->downlink.data == NULL) {
    h2_pal_mem_free(config->allocator, track->downlink.data);
    h2_pal_mem_free(config->allocator, track->uplink.data);
    h2_pal_mem_free(config->allocator, track);
    return H2_PAL_ERR_NO_MEMORY;
  }
  track->uplink.capacity = uplink;
  track->downlink.capacity = downlink;
  *out_track = track;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_pcm_track_destroy(h2_gizclaw_track_t **track) {
  if (track == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  if (*track == NULL)
    return H2_PAL_OK;
  if (atomic_load_explicit(&(*track)->bound, memory_order_acquire))
    return H2_PAL_ERR_BUSY;
  const h2_pal_mem_api_t *allocator = (*track)->allocator;
  h2_pal_mem_free(allocator, (*track)->downlink.data);
  h2_pal_mem_free(allocator, (*track)->uplink.data);
  h2_pal_mem_free(allocator, *track);
  *track = NULL;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_pcm_track_write(h2_gizclaw_track_t *track,
                                           const uint8_t *pcm, size_t len) {
  if (track == NULL || pcm == NULL || len == 0u || (len & 1u) != 0u ||
      len > track->uplink.capacity)
    return H2_PAL_ERR_INVALID_ARG;
  return fake_pcm_ring_write(&track->uplink, pcm, len);
}

h2_pal_result_t h2_gizclaw_pcm_track_read(h2_gizclaw_track_t *track,
                                          uint8_t *pcm, size_t len) {
  if (track == NULL || pcm == NULL || len == 0u || (len & 1u) != 0u ||
      len > track->downlink.capacity)
    return H2_PAL_ERR_INVALID_ARG;
  return fake_pcm_ring_read(&track->downlink, pcm, len);
}

static inline h2_pal_result_t fake_pcm_track_bind(h2_gizclaw_track_t *track) {
  if (track == NULL)
    return H2_PAL_ERR_BUSY;
  bool expected = false;
  if (!atomic_compare_exchange_strong_explicit(
          &track->bound, &expected, true, memory_order_acq_rel,
          memory_order_acquire))
    return H2_PAL_ERR_BUSY;
  return H2_PAL_OK;
}

static inline void fake_pcm_track_unbind(h2_gizclaw_track_t *track) {
  atomic_store_explicit(&track->bound, false, memory_order_release);
}

static inline h2_pal_result_t
fake_pcm_track_service_read(h2_gizclaw_track_t *track, uint8_t *pcm,
                            size_t len) {
  return fake_pcm_ring_read(&track->uplink, pcm, len);
}

static inline h2_pal_result_t
fake_pcm_track_service_write(h2_gizclaw_track_t *track, const uint8_t *pcm,
                             size_t len) {
  return fake_pcm_ring_write(&track->downlink, pcm, len);
}

static inline size_t
fake_pcm_track_uplink_pending(const h2_gizclaw_track_t *track) {
  return atomic_load_explicit(&track->uplink.used, memory_order_acquire);
}

#endif
