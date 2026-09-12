#ifndef H2_GIZCLAW_OGG_OPUS_INTERNAL_H
#define H2_GIZCLAW_OGG_OPUS_INTERNAL_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stdbool.h>
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
/* Blocking sequential reader: OK must supply 1..capacity bytes, EXIT means EOF.
 * Streaming retains one Ogg page and at most 64 KiB of a continued packet;
 * total track length is unrestricted. An OpusTags packet above 64 KiB (large
 * embedded cover art) is accepted on its "OpusTags" magic and the rest is
 * discarded as it streams in; an audio packet above 64 KiB is NO_SPACE. Reader and user
 * live until destroy. */
typedef h2_pal_result_t (*h2_gizclaw_ogg_opus_read_fn)(void *user, uint8_t *out,
                                                       size_t capacity,
                                                       size_t *out_len);
h2_pal_result_t
h2_gizclaw_ogg_opus_create_reader(const h2_pal_mem_api_t *allocator,
                                  h2_gizclaw_ogg_opus_read_fn read, void *user,
                                  h2_gizclaw_ogg_opus_t **out);
h2_pal_result_t h2_gizclaw_ogg_opus_next(h2_gizclaw_ogg_opus_t *decoder,
                                         uint8_t *pcm, size_t capacity,
                                         size_t *out_len);
/* True once OpusHead and OpusTags are parsed and no audio page has been
 * read: the only moment seek() is accepted. A reader has then consumed
 * exactly the header pages, nothing more. */
bool h2_gizclaw_ogg_opus_headers_done(const h2_gizclaw_ogg_opus_t *decoder);
/* Start output at target_ms (16 kHz output timeline, pre-skip removed).
 * Packets ending more than 80 ms before the target are parsed and validated
 * but not decoded; the Opus state is reset before the first decoded packet
 * and samples before the target are discarded.
 * With resync the reader has been switched to a stream that begins at an
 * arbitrary byte of the same file: the decoder scans for the next "OggS",
 * accepts only a complete non-BOS page of this stream with a valid CRC,
 * anchors its timeline on the first such page that is not EOS and has a
 * packet starting on it (granule minus the durations of the packets
 * completed there), and pre-rolls 80 ms from that point. No page before the
 * end, or more than two maximum pages of unusable bytes, is FORMAT.
 * INVALID_STATE unless headers_done(); resync needs a reader decoder. */
h2_pal_result_t h2_gizclaw_ogg_opus_seek(h2_gizclaw_ogg_opus_t *decoder,
                                         uint64_t target_ms, bool resync);
/* Output-timeline index (16 kHz samples) of the first sample next() emits,
 * counted exactly as a playback from the start would count it. Always 0 and
 * true without a seek; false while a seek has not produced its first sample.
 * A stream that ends (or reaches a chained stream) before the target
 * reports that boundary. */
bool h2_gizclaw_ogg_opus_origin(const h2_gizclaw_ogg_opus_t *decoder,
                                uint64_t *samples);
void h2_gizclaw_ogg_opus_destroy(h2_gizclaw_ogg_opus_t *decoder);

#endif
