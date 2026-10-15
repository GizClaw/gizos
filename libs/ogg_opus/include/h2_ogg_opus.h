#ifndef H2_OGG_OPUS_H
#define H2_OGG_OPUS_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_ogg_opus h2_ogg_opus_t;

#define H2_OGG_OPUS_PCM_BYTES (1920u * 2u)

/** @brief Sequential Ogg/Opus family-0 decoder. Borrows immutable input until destroy.
 * Output is 16 kHz mono signed PCM16LE, at most one packet per next(). Header
 * packets, continued pages and completely trimmed audio return OK with zero
 * bytes. Work per call is bounded to one packet or page. EXIT means
 * a validated EOS (including every chained stream), not a truncated input.
 * Pre-skip rounds up and end trimming rounds down to the 16 kHz sample grid.
 * No user callbacks or I/O; all owned memory uses the supplied PAL allocator.
 */
h2_pal_result_t h2_ogg_opus_create(const h2_pal_mem_api_t *allocator,
                                           const uint8_t *data, size_t len,
                                           h2_ogg_opus_t **out);
/** @brief Blocking sequential reader: OK must supply 1..capacity bytes, EXIT means EOF.
 * Streaming retains one Ogg page and at most 64 KiB of a continued packet;
 * total track length is unrestricted. An OpusTags packet above 64 KiB (large
 * embedded cover art) is accepted on its "OpusTags" magic and the rest is
 * discarded as it streams in; an audio packet above 64 KiB is NO_SPACE. Reader and user
 * live until destroy. */
typedef h2_pal_result_t (*h2_ogg_opus_read_fn)(void *user, uint8_t *out,
                                                       size_t capacity,
                                                       size_t *out_len);
/** @brief Create a sequential PCM decoder over a borrowed blocking reader.
 * All owned allocations use allocator; read/user must live until destroy.
 * Sets *out=NULL on failure, performs no reader I/O during creation, and
 * returns INVALID_ARG, NO_MEMORY, or OK. Other callbacks run inside next().
 */
h2_pal_result_t
h2_ogg_opus_create_reader(const h2_pal_mem_api_t *allocator,
                                  h2_ogg_opus_read_fn read, void *user,
                                  h2_ogg_opus_t **out);
/** @brief Decode at most one packet/page into caller-owned PCM storage.
 * Requires capacity >= H2_OGG_OPUS_PCM_BYTES. Clears *out_len on failure;
 * OK may emit zero bytes for headers/preroll. Errors latch until destroy.
 * Performs synchronous reader callbacks, owns no output buffer and is not
 * concurrent-safe. EXIT means the entire validated input has ended.
 */
h2_pal_result_t h2_ogg_opus_next(h2_ogg_opus_t *decoder,
                                         uint8_t *pcm, size_t capacity,
                                         size_t *out_len);
/** @brief True once OpusHead and OpusTags are parsed and no audio page has been
 * read: the only moment seek() is accepted. A reader has then consumed
 * exactly the header pages, nothing more. */
bool h2_ogg_opus_headers_done(const h2_ogg_opus_t *decoder);
/** @brief Start output at target_ms (16 kHz output timeline, pre-skip removed).
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
h2_pal_result_t h2_ogg_opus_seek(h2_ogg_opus_t *decoder,
                                         uint64_t target_ms, bool resync);
/** @brief Output-timeline index (16 kHz samples) of the first sample next() emits,
 * counted exactly as a playback from the start would count it. Always 0 and
 * true without a seek; false while a seek has not produced its first sample.
 * A stream that ends (or reaches a chained stream) before the target
 * reports that boundary. */
bool h2_ogg_opus_origin(const h2_ogg_opus_t *decoder,
                                uint64_t *samples);
/** @brief Release decoder-owned memory; NULL is accepted.
 * Does not close borrowed input or allocator. All reader calls must have
 * returned before destroy; the instance cannot be used afterwards.
 */
void h2_ogg_opus_destroy(h2_ogg_opus_t *decoder);

/** @brief A borrowed encoded audio packet, valid until the next reader call.
 * Timing is on the 16 kHz output grid. Header/empty pages return bytes=0.
 * skip_samples and samples describe RFC 7845 pre-skip and end trimming.
 * stream_start identifies OpusHead and supplies channels/gain with bytes=0.
 */
typedef struct h2_ogg_opus_packet {
  const uint8_t *data;
  size_t bytes;
  uint16_t frame_samples, skip_samples, samples;
  uint8_t channels;
  int16_t gain_q8;
  bool stream_start;
} h2_ogg_opus_packet_t;

/** @brief Create a bounded encoded-packet reader using the existing parser.
 * No Opus decoder or whole-track buffer is allocated. Borrows allocator/read/user
 * until destroy; read is blocking and called only by next_packet(). page_bytes
 * is 282..65307 and packet_bytes is 512..65536. Oversized pages/audio return
 * NO_SPACE; large comment headers keep the existing bounded discard policy.
 * Calls on one instance must be serialized. Partial failure cleans up and
 * sets *out=NULL. Returns INVALID_ARG, NO_MEMORY, or OK.
 */
h2_pal_result_t h2_ogg_opus_create_packet_reader(
    const h2_pal_mem_api_t *allocator, h2_ogg_opus_read_fn read, void *user,
    size_t page_bytes, size_t packet_bytes, h2_ogg_opus_t **out);
/** @brief Validate/return at most one encoded packet or page, without decoding.
 * Clears *out on failure; OK may return bytes=0 for headers/continued pages.
 * EXIT requires validated EOS/EOF. Other errors latch until restore/reset.
 */
h2_pal_result_t h2_ogg_opus_next_packet(h2_ogg_opus_t *reader,
                                         h2_ogg_opus_packet_t *out);
/** @brief Reset a packet reader after its caller rewinds the input to byte 0.
 * Retains bounded buffers, owns no file and performs no I/O. Packet mode only.
 */
h2_pal_result_t h2_ogg_opus_reset_packets(h2_ogg_opus_t *reader);
/** @brief Fixed checkpoint storage required by a packet reader; zero otherwise.
 * Caller owns the byte buffer. It is opaque, same-instance/process storage,
 * must not be modified or persisted, and does not borrow any output packet.
 */
size_t h2_ogg_opus_checkpoint_size(const h2_ogg_opus_t *reader);
/** @brief Save parser state and the input byte position, without I/O.
 * buffer must have checkpoint_size bytes. Clears *position on failure.
 * The caller must seek its reader to this position before restoring.
 */
h2_pal_result_t h2_ogg_opus_checkpoint_save(
    const h2_ogg_opus_t *reader, void *buffer, size_t bytes, uint64_t *position);
/** @brief Restore a previously saved same-reader checkpoint after input seek.
 * No callbacks or allocation; buffers/config remain owned by reader. Packet
 * pointers from any prior next call are invalid. Returns INVALID_ARG for a
 * wrong reader, buffer size or invalid checkpoint; OK restores error recovery.
 */
h2_pal_result_t h2_ogg_opus_checkpoint_restore(
    h2_ogg_opus_t *reader, const void *buffer, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif
