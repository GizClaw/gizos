#ifndef H2_GIZCLAW_PCM_RING_H
#define H2_GIZCLAW_PCM_RING_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct h2_gizclaw_pcm_ring {
  const h2_pal_mem_api_t *allocator;
  uint8_t *bytes;
  size_t capacity;
  atomic_size_t write_index;
  atomic_size_t read_index;
  atomic_bool closed;
} h2_gizclaw_pcm_ring_t;

h2_pal_result_t h2_gizclaw_pcm_ring_init(h2_gizclaw_pcm_ring_t *ring,
                                         const h2_pal_mem_api_t *allocator,
                                         size_t capacity);
void h2_gizclaw_pcm_ring_close(h2_gizclaw_pcm_ring_t *ring);
void h2_gizclaw_pcm_ring_deinit(h2_gizclaw_pcm_ring_t *ring);
size_t h2_gizclaw_pcm_ring_available(const h2_gizclaw_pcm_ring_t *ring);

/**
 * Append PCM without waiting.
 *
 * When there is not enough room, return WOULD_BLOCK without consuming or
 * overwriting any data. This ring has exactly one producer and one consumer.
 */
h2_pal_result_t h2_gizclaw_pcm_ring_write(h2_gizclaw_pcm_ring_t *ring,
                                          const uint8_t *pcm, size_t pcm_len);

/**
 * Append PCM without waiting, dropping the new chunk when the ring is full.
 *
 * The call is safe for exactly one producer and one consumer.
 * out_dropped_bytes reports the discarded new chunk. The producer never
 * advances the consumer index, so the byte storage remains data-race free.
 */
h2_pal_result_t h2_gizclaw_pcm_ring_write_latest(h2_gizclaw_pcm_ring_t *ring,
                                                 const uint8_t *pcm,
                                                 size_t pcm_len,
                                                 size_t *out_dropped_bytes);

/** Read exactly pcm_len bytes, or return WOULD_BLOCK without consuming. */
h2_pal_result_t h2_gizclaw_pcm_ring_read(h2_gizclaw_pcm_ring_t *ring,
                                         uint8_t *pcm, size_t pcm_len);

#endif
