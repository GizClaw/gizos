#include "h2_gizclaw_pcm_ring.h"

#include <string.h>

h2_pal_result_t h2_gizclaw_pcm_ring_init(h2_gizclaw_pcm_ring_t *ring,
                                         const h2_pal_mem_api_t *allocator,
                                         size_t capacity) {
  if (ring == NULL || allocator == NULL || capacity == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  memset(ring, 0, sizeof(*ring));
  ring->allocator = allocator;
  ring->capacity = capacity;
  ring->bytes = h2_pal_mem_alloc(allocator, capacity);
  if (ring->bytes == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  if (h2_atomic_size_init(&ring->write_index, 0u) != H2_ATOMIC_OK ||
      h2_atomic_size_init(&ring->read_index, 0u) != H2_ATOMIC_OK ||
      h2_atomic_bool_init(&ring->closed, false) != H2_ATOMIC_OK) {
    h2_gizclaw_pcm_ring_deinit(ring);
    return H2_PAL_ERR_NO_MEMORY;
  }
  return H2_PAL_OK;
}

void h2_gizclaw_pcm_ring_close(h2_gizclaw_pcm_ring_t *ring) {
  if (ring != NULL)
    h2_atomic_bool_store(&ring->closed, true, H2_ATOMIC_RELEASE);
}

void h2_gizclaw_pcm_ring_deinit(h2_gizclaw_pcm_ring_t *ring) {
  if (ring == NULL || ring->bytes == NULL)
    return;
  h2_atomic_size_destroy(&ring->write_index);
  h2_atomic_size_destroy(&ring->read_index);
  h2_atomic_bool_destroy(&ring->closed);
  h2_pal_mem_free(ring->allocator, ring->bytes);
  memset(ring, 0, sizeof(*ring));
}

size_t h2_gizclaw_pcm_ring_available(const h2_gizclaw_pcm_ring_t *ring) {
  if (ring == NULL || ring->bytes == NULL)
    return 0u;
  const size_t write =
      h2_atomic_size_load(&ring->write_index, H2_ATOMIC_ACQUIRE);
  const size_t read =
      h2_atomic_size_load(&ring->read_index, H2_ATOMIC_ACQUIRE);
  return write - read;
}

h2_pal_result_t h2_gizclaw_pcm_ring_write(h2_gizclaw_pcm_ring_t *ring,
                                          const uint8_t *pcm, size_t pcm_len) {
  if (ring == NULL || ring->bytes == NULL || pcm == NULL || pcm_len == 0u ||
      pcm_len > ring->capacity)
    return H2_PAL_ERR_INVALID_ARG;
  if (h2_atomic_bool_load(&ring->closed, H2_ATOMIC_ACQUIRE))
    return H2_PAL_ERR_CLOSED;
  const size_t write =
      h2_atomic_size_load(&ring->write_index, H2_ATOMIC_RELAXED);
  const size_t read =
      h2_atomic_size_load(&ring->read_index, H2_ATOMIC_ACQUIRE);
  const size_t used = write - read;
  if (used > ring->capacity || ring->capacity - used < pcm_len)
    return H2_PAL_ERR_WOULD_BLOCK;

  const size_t offset = write % ring->capacity;
  size_t first = ring->capacity - offset;
  if (first > pcm_len)
    first = pcm_len;
  memcpy(ring->bytes + offset, pcm, first);
  if (first < pcm_len)
    memcpy(ring->bytes, pcm + first, pcm_len - first);
  h2_atomic_size_store(&ring->write_index, write + pcm_len, H2_ATOMIC_RELEASE);
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_pcm_ring_read(h2_gizclaw_pcm_ring_t *ring,
                                         uint8_t *pcm, size_t pcm_len) {
  if (ring == NULL || ring->bytes == NULL || pcm == NULL || pcm_len == 0u ||
      pcm_len > ring->capacity)
    return H2_PAL_ERR_INVALID_ARG;
  const size_t read =
      h2_atomic_size_load(&ring->read_index, H2_ATOMIC_RELAXED);
  const size_t write =
      h2_atomic_size_load(&ring->write_index, H2_ATOMIC_ACQUIRE);
  if (write - read < pcm_len) {
    return h2_atomic_bool_load(&ring->closed, H2_ATOMIC_ACQUIRE)
               ? H2_PAL_ERR_CLOSED
               : H2_PAL_ERR_WOULD_BLOCK;
  }
  const size_t offset = read % ring->capacity;
  size_t first = ring->capacity - offset;
  if (first > pcm_len)
    first = pcm_len;
  memcpy(pcm, ring->bytes + offset, first);
  if (first < pcm_len)
    memcpy(pcm + first, ring->bytes, pcm_len - first);
  h2_atomic_size_store(&ring->read_index, read + pcm_len, H2_ATOMIC_RELEASE);
  return H2_PAL_OK;
}
