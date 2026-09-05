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
  atomic_init(&ring->write_index, 0u);
  atomic_init(&ring->read_index, 0u);
  atomic_init(&ring->closed, false);
  return H2_PAL_OK;
}

void h2_gizclaw_pcm_ring_close(h2_gizclaw_pcm_ring_t *ring) {
  if (ring != NULL)
    atomic_store_explicit(&ring->closed, true, memory_order_release);
}

void h2_gizclaw_pcm_ring_deinit(h2_gizclaw_pcm_ring_t *ring) {
  if (ring == NULL || ring->bytes == NULL)
    return;
  h2_pal_mem_free(ring->allocator, ring->bytes);
  memset(ring, 0, sizeof(*ring));
}

size_t h2_gizclaw_pcm_ring_available(const h2_gizclaw_pcm_ring_t *ring) {
  if (ring == NULL || ring->bytes == NULL)
    return 0u;
  const size_t write =
      atomic_load_explicit(&ring->write_index, memory_order_acquire);
  const size_t read =
      atomic_load_explicit(&ring->read_index, memory_order_acquire);
  return write - read;
}

h2_pal_result_t h2_gizclaw_pcm_ring_write(h2_gizclaw_pcm_ring_t *ring,
                                          const uint8_t *pcm, size_t pcm_len) {
  if (ring == NULL || ring->bytes == NULL || pcm == NULL || pcm_len == 0u ||
      pcm_len > ring->capacity)
    return H2_PAL_ERR_INVALID_ARG;
  if (atomic_load_explicit(&ring->closed, memory_order_acquire))
    return H2_PAL_ERR_CLOSED;
  const size_t write =
      atomic_load_explicit(&ring->write_index, memory_order_relaxed);
  const size_t read =
      atomic_load_explicit(&ring->read_index, memory_order_acquire);
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
  atomic_store_explicit(&ring->write_index, write + pcm_len,
                        memory_order_release);
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_pcm_ring_read(h2_gizclaw_pcm_ring_t *ring,
                                         uint8_t *pcm, size_t pcm_len) {
  if (ring == NULL || ring->bytes == NULL || pcm == NULL || pcm_len == 0u ||
      pcm_len > ring->capacity)
    return H2_PAL_ERR_INVALID_ARG;
  const size_t read =
      atomic_load_explicit(&ring->read_index, memory_order_relaxed);
  const size_t write =
      atomic_load_explicit(&ring->write_index, memory_order_acquire);
  if (write - read < pcm_len) {
    return atomic_load_explicit(&ring->closed, memory_order_acquire)
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
  atomic_store_explicit(&ring->read_index, read + pcm_len,
                        memory_order_release);
  return H2_PAL_OK;
}
