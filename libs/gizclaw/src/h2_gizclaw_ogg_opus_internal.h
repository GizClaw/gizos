#ifndef H2_GIZCLAW_OGG_OPUS_INTERNAL_H
#define H2_GIZCLAW_OGG_OPUS_INTERNAL_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stdint.h>

typedef struct h2_gizclaw_ogg_opus h2_gizclaw_ogg_opus_t;

#define H2_GIZCLAW_OGG_OPUS_PCM_BYTES (1920u * 2u)

/* Sequential Ogg/Opus family-0 decoder. Borrows immutable input until destroy.
 * Output is 16 kHz mono signed PCM16LE, at most one packet per next(). Header
 * packets, continued pages and completely trimmed audio return OK with zero
 * bytes. Work per call is bounded to one packet or page. EXIT means
 * a validated EOS (including every chained stream), not a truncated input.
 * Pre-skip rounds up and end trimming rounds down to the 16 kHz sample grid.
 * No user callbacks or I/O; all owned memory uses the supplied PAL allocator.
 */
h2_pal_result_t h2_gizclaw_ogg_opus_create(const h2_pal_mem_api_t *allocator,
                                           const uint8_t *data, size_t len,
                                           h2_gizclaw_ogg_opus_t **out);
h2_pal_result_t h2_gizclaw_ogg_opus_next(h2_gizclaw_ogg_opus_t *decoder,
                                         uint8_t *pcm, size_t capacity,
                                         size_t *out_len);
void h2_gizclaw_ogg_opus_destroy(h2_gizclaw_ogg_opus_t *decoder);

#endif
