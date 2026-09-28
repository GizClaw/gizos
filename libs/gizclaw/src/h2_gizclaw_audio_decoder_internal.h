#ifndef H2_GIZCLAW_AUDIO_DECODER_INTERNAL_H
#define H2_GIZCLAW_AUDIO_DECODER_INTERNAL_H

#include "h2_gizclaw_ogg_opus_internal.h"

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The player's decoder. The first bytes pick the format: "OggS" is
 * Ogg/Opus, "RIFF....WAVE" is WAV, and "ID3" or an MPEG audio Layer III
 * frame header is MP3; anything else is UNSUPPORTED before any audio. Every
 * format yields 16 kHz mono signed PCM16LE, at most PCM_BYTES per next();
 * MP3 and WAV are mixed down to mono and resampled to 16 kHz. OK with zero
 * bytes is normal (headers, skipped data). EXIT is the end of the track.
 * Owned memory comes from the PAL allocator; there are no callbacks. */
#define H2_GIZCLAW_AUDIO_PCM_BYTES H2_GIZCLAW_OGG_OPUS_PCM_BYTES

/* Blocking sequential reader: OK supplies 1..capacity bytes, EXIT is EOF. */
typedef h2_gizclaw_ogg_opus_read_fn h2_gizclaw_audio_read_fn;

typedef struct h2_gizclaw_audio_decoder h2_gizclaw_audio_decoder_t;

h2_pal_result_t h2_gizclaw_audio_decoder_create(
    const h2_pal_mem_api_t *allocator, h2_gizclaw_audio_read_fn read,
    void *user, h2_gizclaw_audio_decoder_t **out);
h2_pal_result_t h2_gizclaw_audio_decoder_next(h2_gizclaw_audio_decoder_t *d,
                                              uint8_t *pcm, size_t capacity,
                                              size_t *out_len);
/* True from the moment the headers are parsed until the first audio is
 * read: the only window in which seek_offset() and seek() are accepted. */
bool h2_gizclaw_audio_decoder_headers_done(const h2_gizclaw_audio_decoder_t *d);
/* Where a ranged request should begin, in a file of `total` bytes, to start
 * output at target_ms. duration_ms is the length the product knows; only
 * Ogg/Opus needs it. Ogg/Opus estimates by byte rate 5 s early and lands on
 * the next page; WAV is exact; MP3 is exact for a CBR stream with a LAME
 * "Info" tag and otherwise estimates 5 s early from the Xing TOC, the Xing
 * frame and byte counts or the first frame's bitrate. FORMAT when that lies
 * at or past the end, which the caller treats as "cannot range". */
h2_pal_result_t h2_gizclaw_audio_decoder_seek_offset(
    h2_gizclaw_audio_decoder_t *d, uint64_t target_ms, uint64_t duration_ms,
    uint64_t total, uint64_t *offset);
/* Start output at target_ms. Without resync the current stream is read on
 * and skipped through (Ogg pages and MP3 frames are parsed, not decoded).
 * With resync the reader now delivers the file from `offset`, the value
 * seek_offset() returned: Ogg/Opus resynchronises on the next valid page,
 * MP3 on the next pair of consistent frame headers, and WAV continues at the
 * exact frame. Output before the target is decoded as pre-roll and dropped.
 * INVALID_STATE outside headers_done(). */
h2_pal_result_t h2_gizclaw_audio_decoder_seek(h2_gizclaw_audio_decoder_t *d,
                                              uint64_t target_ms, bool resync,
                                              uint64_t offset);
/* Output-timeline index (16 kHz samples) of the first sample next() emits
 * after a seek; 0 and true without one, false while a seek has not produced
 * its first sample. A track that ends before the target reports its end.
 * Exact for Ogg/Opus and WAV and for MP3 without resync; after an MP3
 * resync it is the landing frame's estimated position. */
bool h2_gizclaw_audio_decoder_origin(const h2_gizclaw_audio_decoder_t *d,
                                     uint64_t *samples);
void h2_gizclaw_audio_decoder_destroy(h2_gizclaw_audio_decoder_t *d);

/* Below: the pieces the MP3 and WAV parsers share with the front end. */

/* Buffered view of the reader. position is the file offset of the next
 * unread byte. */
#define H2_GIZCLAW_AUDIO_INPUT_BYTES 4096u
typedef struct h2_gizclaw_audio_input {
  h2_gizclaw_audio_read_fn read;
  void *user;
  uint64_t position;
  size_t offset, len; /* Unread bytes are data[offset, offset + len). */
  bool eof;
  uint8_t data[H2_GIZCLAW_AUDIO_INPUT_BYTES];
} h2_gizclaw_audio_input_t;

/* OK once at least `need` (<= INPUT_BYTES) unread bytes are buffered; EXIT
 * when the stream ends first, with whatever arrived still readable. */
h2_pal_result_t h2_gizclaw_audio_input_fill(h2_gizclaw_audio_input_t *in,
                                            size_t need);
void h2_gizclaw_audio_input_consume(h2_gizclaw_audio_input_t *in,
                                    size_t count);
/* Discard `count` bytes, reading through the stream; EXIT if it ends first. */
h2_pal_result_t h2_gizclaw_audio_input_skip(h2_gizclaw_audio_input_t *in,
                                            uint64_t count);
/* The reader now delivers the file from `position` on. */
void h2_gizclaw_audio_input_restart(h2_gizclaw_audio_input_t *in,
                                    uint64_t position);

/* Mono PCM16 at the source rate. start indexes samples[0] on the track's
 * own timeline (encoder delay removed); a chunk that does not continue the
 * previous one starts a new resampler run. */
typedef struct h2_gizclaw_audio_chunk {
  const int16_t *samples;
  size_t count;
  uint64_t start;
} h2_gizclaw_audio_chunk_t;

/* MPEG-1/2/2.5 Layer III. Frames are delimited here and decoded one at a
 * time by dr_mp3; free-format and Layer I/II streams are UNSUPPORTED. ID3v2
 * tags of any size stream past; a Xing, Info or VBRI frame is metadata, and
 * a LAME tag's encoder delay and padding are trimmed. A frame of this
 * stream is accepted where one is expected; elsewhere (the first frame,
 * lost sync, a resync) it must be followed by a consistent header or the
 * end of the stream. The end of the stream ends the track: MP3 carries no
 * end marker, so a truncated file ends at its last whole frame. */
typedef struct h2_gizclaw_mp3 h2_gizclaw_mp3_t;
bool h2_gizclaw_mp3_header_valid(const uint8_t header[4]);
h2_pal_result_t h2_gizclaw_mp3_create(const h2_pal_mem_api_t *allocator,
                                      h2_gizclaw_audio_input_t *input,
                                      h2_gizclaw_mp3_t **out);
/* One frame per call. EXIT at the end, with *end the track length. */
h2_pal_result_t h2_gizclaw_mp3_next(h2_gizclaw_mp3_t *m,
                                    h2_gizclaw_audio_chunk_t *chunk,
                                    uint64_t *end);
uint32_t h2_gizclaw_mp3_rate(const h2_gizclaw_mp3_t *m);
bool h2_gizclaw_mp3_headers_done(const h2_gizclaw_mp3_t *m);
/* target is a sample index on the track's timeline. */
h2_pal_result_t h2_gizclaw_mp3_seek_offset(const h2_gizclaw_mp3_t *m,
                                           uint64_t target, uint64_t total,
                                           uint64_t *offset);
h2_pal_result_t h2_gizclaw_mp3_seek(h2_gizclaw_mp3_t *m, uint64_t target,
                                    bool resync);
void h2_gizclaw_mp3_destroy(h2_gizclaw_mp3_t *m);

/* RIFF/WAVE with integer PCM of 8, 16, 24 or 32 bits or 32-bit float
 * (plain or WAVE_FORMAT_EXTENSIBLE), 1..8 channels at 8..48 kHz; other
 * encodings are UNSUPPORTED. Chunks before "data" stream past; a data size
 * of 0 or 0xFFFFFFFF means "until the end of the stream". */
typedef struct h2_gizclaw_wav h2_gizclaw_wav_t;
h2_pal_result_t h2_gizclaw_wav_create(const h2_pal_mem_api_t *allocator,
                                      h2_gizclaw_audio_input_t *input,
                                      h2_gizclaw_wav_t **out);
/* At most max_samples frames per call. EXIT at the end, with *end the
 * track length. */
h2_pal_result_t h2_gizclaw_wav_next(h2_gizclaw_wav_t *w, size_t max_samples,
                                    h2_gizclaw_audio_chunk_t *chunk,
                                    uint64_t *end);
uint32_t h2_gizclaw_wav_rate(const h2_gizclaw_wav_t *w);
bool h2_gizclaw_wav_headers_done(const h2_gizclaw_wav_t *w);
h2_pal_result_t h2_gizclaw_wav_seek_offset(const h2_gizclaw_wav_t *w,
                                           uint64_t target, uint64_t total,
                                           uint64_t *offset);
h2_pal_result_t h2_gizclaw_wav_seek(h2_gizclaw_wav_t *w, uint64_t target,
                                    bool resync);
void h2_gizclaw_wav_destroy(h2_gizclaw_wav_t *w);

#endif
