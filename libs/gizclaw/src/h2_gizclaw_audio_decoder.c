#include "h2_gizclaw_audio_decoder_internal.h"

#include "h2_gizclaw_resample_internal.h"

#include <string.h>

/* Aim a byte-rate estimate this far before the start so the landing page is
 * earlier and the decoder skips to the exact start instead of overshooting it
 * by the error of the estimate. On a real 7.5 min VBR speech file with 1 s
 * pages the estimate drifted up to ~2 s; 5 s landed early for every start at
 * a cost of a few seconds of skipped bytes. */
#define OGG_SEEK_BACKOFF_MS 5000u
#define OUTPUT_RATE H2_GIZCLAW_RESAMPLE_OUTPUT_RATE
#define OUTPUT_SAMPLES (H2_GIZCLAW_AUDIO_PCM_BYTES / 2u)

typedef enum audio_format {
  FORMAT_UNKNOWN,
  FORMAT_OGG,
  FORMAT_MP3,
  FORMAT_WAV,
} audio_format_t;

/* MP3 and WAV source PCM onto the 16 kHz output timeline. next16 is the
 * output index of the resampler's next sample; while seeking, samples
 * before target16 are dropped and the first one kept is the origin. */
typedef struct pcm_stage {
  uint32_t rate;
  bool running, flushed;
  uint64_t expected, next16, target16, origin16;
  bool seeking;
  h2_gizclaw_resample_t resample;
  int16_t out[OUTPUT_SAMPLES];
} pcm_stage_t;

struct h2_gizclaw_audio_decoder {
  const h2_pal_mem_api_t *allocator;
  audio_format_t format;
  h2_pal_result_t error;
  h2_gizclaw_ogg_opus_t *ogg;
  h2_gizclaw_mp3_t *mp3;
  h2_gizclaw_wav_t *wav;
  pcm_stage_t *stage;
  /* Bytes handed to the Ogg/Opus decoder: its headers' length once they are
   * parsed, which places a byte-rate estimate. */
  uint64_t delivered;
  h2_gizclaw_audio_input_t input;
};

h2_pal_result_t h2_gizclaw_audio_input_fill(h2_gizclaw_audio_input_t *in,
                                            size_t need) {
  if (need > H2_GIZCLAW_AUDIO_INPUT_BYTES)
    return H2_PAL_ERR_INVALID_ARG;
  if (in->len >= need)
    return H2_PAL_OK;
  if (in->eof)
    return H2_PAL_EXIT;
  if (in->offset) {
    memmove(in->data, in->data + in->offset, in->len);
    in->offset = 0;
  }
  while (in->len < need) {
    const size_t room = H2_GIZCLAW_AUDIO_INPUT_BYTES - in->len;
    size_t count = 0;
    const h2_pal_result_t rc =
        in->read(in->user, in->data + in->len, room, &count);
    if (rc == H2_PAL_EXIT) {
      in->eof = true;
      return H2_PAL_EXIT;
    }
    if (rc != H2_PAL_OK)
      return rc;
    if (!count || count > room)
      return H2_PAL_ERR_FORMAT;
    in->len += count;
  }
  return H2_PAL_OK;
}

void h2_gizclaw_audio_input_consume(h2_gizclaw_audio_input_t *in,
                                    size_t count) {
  in->offset += count;
  in->len -= count;
  in->position += count;
}

h2_pal_result_t h2_gizclaw_audio_input_skip(h2_gizclaw_audio_input_t *in,
                                            uint64_t count) {
  while (count) {
    if (!in->len) {
      const h2_pal_result_t rc = h2_gizclaw_audio_input_fill(in, 1);
      if (rc != H2_PAL_OK)
        return rc;
    }
    const size_t take = count < in->len ? (size_t)count : in->len;
    h2_gizclaw_audio_input_consume(in, take);
    count -= take;
  }
  return H2_PAL_OK;
}

void h2_gizclaw_audio_input_restart(h2_gizclaw_audio_input_t *in,
                                    uint64_t position) {
  in->offset = in->len = 0;
  in->eof = false;
  in->position = position;
}

/* The Ogg/Opus decoder reads through the sniffed bytes first. */
static h2_pal_result_t ogg_read(void *user, uint8_t *out, size_t capacity,
                                size_t *out_len) {
  h2_gizclaw_audio_decoder_t *d = user;
  h2_gizclaw_audio_input_t *in = &d->input;
  *out_len = 0;
  if (!in->len) {
    if (in->eof)
      return H2_PAL_EXIT;
    const h2_pal_result_t rc = in->read(in->user, out, capacity, out_len);
    if (rc == H2_PAL_OK) {
      in->position += *out_len;
      d->delivered += *out_len;
    }
    return rc;
  }
  const size_t count = capacity < in->len ? capacity : in->len;
  memcpy(out, in->data + in->offset, count);
  h2_gizclaw_audio_input_consume(in, count);
  d->delivered += count;
  *out_len = count;
  return H2_PAL_OK;
}

static h2_pal_result_t sniff(h2_gizclaw_audio_decoder_t *d) {
  h2_gizclaw_audio_input_t *in = &d->input;
  h2_pal_result_t rc = h2_gizclaw_audio_input_fill(in, 12);
  if (rc != H2_PAL_OK && rc != H2_PAL_EXIT)
    return rc;
  const uint8_t *p = in->data + in->offset;
  const size_t n = in->len;
  if (n >= 4u && !memcmp(p, "OggS", 4)) {
    d->format = FORMAT_OGG;
    return h2_gizclaw_ogg_opus_create_reader(d->allocator, ogg_read, d,
                                             &d->ogg);
  }
  if (n >= 12u && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WAVE", 4))
    d->format = FORMAT_WAV;
  else if ((n >= 3u && !memcmp(p, "ID3", 3)) ||
           (n >= 4u && h2_gizclaw_mp3_header_valid(p)))
    d->format = FORMAT_MP3;
  else
    return n ? H2_PAL_ERR_UNSUPPORTED : H2_PAL_ERR_FORMAT;
  d->stage = h2_pal_mem_alloc(d->allocator, sizeof(*d->stage));
  if (d->stage == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(d->stage, 0, sizeof(*d->stage));
  return d->format == FORMAT_MP3
             ? h2_gizclaw_mp3_create(d->allocator, in, &d->mp3)
             : h2_gizclaw_wav_create(d->allocator, in, &d->wav);
}

static uint32_t source_rate(const h2_gizclaw_audio_decoder_t *d) {
  return d->format == FORMAT_MP3 ? h2_gizclaw_mp3_rate(d->mp3)
                                 : h2_gizclaw_wav_rate(d->wav);
}

/* Largest source run whose output fits one next(). */
static size_t source_limit(const pcm_stage_t *s) {
  size_t count = H2_GIZCLAW_RESAMPLE_INPUT_MAX;
  while (count > 1u && h2_gizclaw_resample_capacity(&s->resample, count) >
                           OUTPUT_SAMPLES)
    count -= 64u < count ? 64u : count - 1u;
  return count;
}

/* Keep what lies at or after the target and write it as PCM16LE. */
static void emit(pcm_stage_t *s, size_t count, uint8_t *pcm, size_t *out_len) {
  size_t first = 0;
  if (s->seeking) {
    if (s->next16 + count <= s->target16)
      first = count;
    else {
      first = s->next16 < s->target16 ? (size_t)(s->target16 - s->next16) : 0;
      s->origin16 = s->next16 + first;
      s->seeking = false;
    }
  }
  s->next16 += count;
  for (size_t i = first; i < count; ++i) {
    const uint16_t sample = (uint16_t)s->out[i];
    pcm[(*out_len)++] = (uint8_t)sample;
    pcm[(*out_len)++] = (uint8_t)(sample >> 8);
  }
}

static h2_pal_result_t pcm_next(h2_gizclaw_audio_decoder_t *d, uint8_t *pcm,
                                size_t *out_len) {
  pcm_stage_t *s = d->stage;
  if (s->flushed)
    return H2_PAL_EXIT;
  if (!s->rate && source_rate(d)) {
    s->rate = source_rate(d);
    /* Sized before the first run: the window depends only on the rate. */
    if (!h2_gizclaw_resample_reset(&s->resample, s->rate, 0))
      return H2_PAL_ERR_UNSUPPORTED;
  }
  h2_gizclaw_audio_chunk_t chunk = {0};
  uint64_t end = 0;
  const h2_pal_result_t rc =
      d->format == FORMAT_MP3
          ? h2_gizclaw_mp3_next(d->mp3, &chunk, &end)
          : h2_gizclaw_wav_next(d->wav, s->rate ? source_limit(s) : 1u, &chunk,
                                &end);
  if (rc == H2_PAL_EXIT) {
    s->flushed = true;
    if (s->running)
      emit(s, h2_gizclaw_resample_flush(&s->resample, s->out), pcm, out_len);
    if (s->seeking) {
      /* The track ended before the target: it is over there. */
      s->seeking = false;
      s->origin16 = s->running ? s->next16
                               : (end * OUTPUT_RATE + s->rate - 1u) / s->rate;
    }
    return *out_len ? H2_PAL_OK : H2_PAL_EXIT;
  }
  if (rc != H2_PAL_OK || !chunk.count)
    return rc;
  if (!s->running || chunk.start != s->expected) {
    /* A new run: its first output is the first 16 kHz grid point at or
     * after the chunk's first sample. Sources only jump at a seek, before
     * any audio, so a previous run's tail is never needed. */
    const uint64_t first16 =
        (chunk.start * OUTPUT_RATE + s->rate - 1u) / s->rate;
    const uint32_t phase =
        (uint32_t)(first16 * s->rate - chunk.start * OUTPUT_RATE);
    if (!h2_gizclaw_resample_reset(&s->resample, s->rate, phase))
      return H2_PAL_ERR_FORMAT;
    s->running = true;
    s->next16 = first16;
  }
  s->expected = chunk.start + chunk.count;
  emit(s,
       h2_gizclaw_resample_push(&s->resample, chunk.samples, chunk.count,
                                s->out),
       pcm, out_len);
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_audio_decoder_next(h2_gizclaw_audio_decoder_t *d,
                                              uint8_t *pcm, size_t capacity,
                                              size_t *out_len) {
  if (out_len != NULL)
    *out_len = 0;
  if (d == NULL || pcm == NULL || out_len == NULL ||
      capacity < H2_GIZCLAW_AUDIO_PCM_BYTES)
    return H2_PAL_ERR_INVALID_ARG;
  if (d->error != H2_PAL_OK)
    return d->error;
  h2_pal_result_t rc = H2_PAL_OK;
  if (d->format == FORMAT_UNKNOWN)
    rc = sniff(d);
  if (rc == H2_PAL_OK)
    rc = d->format == FORMAT_OGG
             ? h2_gizclaw_ogg_opus_next(d->ogg, pcm, capacity, out_len)
             : pcm_next(d, pcm, out_len);
  if (rc != H2_PAL_OK && rc != H2_PAL_EXIT)
    d->error = rc;
  return rc;
}

bool h2_gizclaw_audio_decoder_headers_done(
    const h2_gizclaw_audio_decoder_t *d) {
  if (d == NULL || d->error != H2_PAL_OK)
    return false;
  switch (d->format) {
  case FORMAT_OGG:
    return h2_gizclaw_ogg_opus_headers_done(d->ogg);
  case FORMAT_MP3:
    return h2_gizclaw_mp3_headers_done(d->mp3);
  case FORMAT_WAV:
    return h2_gizclaw_wav_headers_done(d->wav);
  default:
    return false;
  }
}

h2_pal_result_t h2_gizclaw_audio_decoder_seek_offset(
    h2_gizclaw_audio_decoder_t *d, uint64_t target_ms, uint64_t duration_ms,
    uint64_t total, uint64_t *offset) {
  if (d == NULL || offset == NULL || target_ms > UINT64_MAX / 48000u)
    return H2_PAL_ERR_INVALID_ARG;
  if (!h2_gizclaw_audio_decoder_headers_done(d))
    return H2_PAL_ERR_INVALID_STATE;
  if (d->format == FORMAT_OGG) {
    if (!duration_ms)
      return H2_PAL_ERR_INVALID_ARG;
    const uint64_t header = d->delivered;
    if (header >= total)
      return H2_PAL_ERR_FORMAT;
    const uint64_t aim =
        target_ms > OGG_SEEK_BACKOFF_MS ? target_ms - OGG_SEEK_BACKOFF_MS : 0;
    *offset = header + (uint64_t)((double)(total - header) * (double)aim /
                                  (double)duration_ms);
    return *offset < total ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
  }
  const uint64_t target = target_ms * source_rate(d) / 1000u;
  return d->format == FORMAT_MP3
             ? h2_gizclaw_mp3_seek_offset(d->mp3, target, total, offset)
             : h2_gizclaw_wav_seek_offset(d->wav, target, total, offset);
}

h2_pal_result_t h2_gizclaw_audio_decoder_seek(h2_gizclaw_audio_decoder_t *d,
                                              uint64_t target_ms, bool resync,
                                              uint64_t offset) {
  if (d == NULL || target_ms > UINT64_MAX / 48000u)
    return H2_PAL_ERR_INVALID_ARG;
  if (!h2_gizclaw_audio_decoder_headers_done(d) ||
      (d->stage != NULL && d->stage->seeking))
    return H2_PAL_ERR_INVALID_STATE;
  if (resync)
    h2_gizclaw_audio_input_restart(&d->input, offset);
  if (d->format == FORMAT_OGG)
    return h2_gizclaw_ogg_opus_seek(d->ogg, target_ms, resync);
  const uint64_t target = target_ms * source_rate(d) / 1000u;
  const h2_pal_result_t rc =
      d->format == FORMAT_MP3 ? h2_gizclaw_mp3_seek(d->mp3, target, resync)
                              : h2_gizclaw_wav_seek(d->wav, target, resync);
  if (rc == H2_PAL_OK) {
    d->stage->seeking = true;
    d->stage->target16 = target_ms * (OUTPUT_RATE / 1000u);
  }
  return rc;
}

bool h2_gizclaw_audio_decoder_origin(const h2_gizclaw_audio_decoder_t *d,
                                     uint64_t *samples) {
  if (d == NULL || samples == NULL)
    return false;
  if (d->format == FORMAT_OGG)
    return h2_gizclaw_ogg_opus_origin(d->ogg, samples);
  if (d->stage != NULL && d->stage->seeking)
    return false;
  *samples = d->stage != NULL ? d->stage->origin16 : 0u;
  return true;
}

h2_pal_result_t h2_gizclaw_audio_decoder_create(
    const h2_pal_mem_api_t *allocator, h2_gizclaw_audio_read_fn read,
    void *user, h2_gizclaw_audio_decoder_t **out) {
  if (out != NULL)
    *out = NULL;
  if (out == NULL || read == NULL || allocator == NULL ||
      allocator->vtable == NULL || allocator->vtable->alloc == NULL ||
      allocator->vtable->free == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_audio_decoder_t *d = h2_pal_mem_alloc(allocator, sizeof(*d));
  if (d == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(d, 0, sizeof(*d));
  d->allocator = allocator;
  d->input.read = read;
  d->input.user = user;
  *out = d;
  return H2_PAL_OK;
}

void h2_gizclaw_audio_decoder_destroy(h2_gizclaw_audio_decoder_t *d) {
  if (d == NULL)
    return;
  h2_gizclaw_ogg_opus_destroy(d->ogg);
  h2_gizclaw_mp3_destroy(d->mp3);
  h2_gizclaw_wav_destroy(d->wav);
  h2_pal_mem_free(d->allocator, d->stage);
  h2_pal_mem_free(d->allocator, d);
}
