#include "h2_gizclaw_audio_decoder_internal.h"

#include "h2_gizclaw_resample_internal.h"

#include <string.h>

/* Frames before a seek target decoded and dropped: a resampler window. */
#define PRE_ROLL_FRAMES 64u
/* Frames converted per call; the front end may ask for fewer. */
#define CHUNK_FRAMES 1024u
/* Bytes skipped per call while seeking through the stream. */
#define SKIP_BYTES 65536u

typedef enum wav_state {
  WAV_HEAD,
  WAV_READY, /* "data" reached, no sample read: seeks are accepted. */
  WAV_PLAY,
  WAV_END,
} wav_state_t;

struct h2_gizclaw_wav {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_audio_input_t *in;
  wav_state_t state;
  h2_pal_result_t error;
  bool is_float;
  uint32_t rate, channels, width, block;
  /* File offset of the first sample and the data length in frames,
   * UINT64_MAX until the end of the stream. */
  uint64_t data, frames;
  uint64_t frame, skip_until;
  int16_t pcm[CHUNK_FRAMES];
};

static uint16_t le16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* "fmt " body of `size` bytes, of which the first min(size, 40) are at p. */
static h2_pal_result_t parse_format(h2_gizclaw_wav_t *w, const uint8_t *p,
                                    uint32_t size) {
  static const uint8_t guid_tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10,
                                        0x00, 0x80, 0x00, 0x00, 0xAA,
                                        0x00, 0x38, 0x9B, 0x71};
  if (size < 16u)
    return H2_PAL_ERR_FORMAT;
  uint16_t tag = le16(p);
  const uint32_t channels = le16(p + 2), rate = le32(p + 4);
  const uint32_t block = le16(p + 12), bits = le16(p + 14);
  if (tag == 0xFFFEu) {
    if (size < 40u || le16(p + 16) < 22u || memcmp(p + 26, guid_tail, 14))
      return H2_PAL_ERR_UNSUPPORTED;
    tag = le16(p + 24);
  }
  const bool integer = tag == 1u && (bits == 8u || bits == 16u ||
                                     bits == 24u || bits == 32u);
  const bool real = tag == 3u && bits == 32u;
  if ((!integer && !real) || channels == 0u || channels > 8u ||
      rate < H2_GIZCLAW_RESAMPLE_RATE_MIN ||
      rate > H2_GIZCLAW_RESAMPLE_RATE_MAX)
    return H2_PAL_ERR_UNSUPPORTED;
  if (block != channels * (bits / 8u))
    return H2_PAL_ERR_FORMAT;
  w->is_float = real;
  w->rate = rate;
  w->channels = channels;
  w->width = bits / 8u;
  w->block = block;
  return H2_PAL_OK;
}

/* RIFF header, then chunks until "data"; odd chunks carry a pad byte. */
static h2_pal_result_t head(h2_gizclaw_wav_t *w) {
  h2_gizclaw_audio_input_t *in = w->in;
  h2_pal_result_t rc = h2_gizclaw_audio_input_fill(in, 12);
  if (rc != H2_PAL_OK)
    return rc == H2_PAL_EXIT ? H2_PAL_ERR_FORMAT : rc;
  const uint8_t *p = in->data + in->offset;
  if (memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4))
    return H2_PAL_ERR_FORMAT;
  h2_gizclaw_audio_input_consume(in, 12);
  bool have_format = false;
  for (;;) {
    rc = h2_gizclaw_audio_input_fill(in, 8);
    if (rc != H2_PAL_OK)
      return rc == H2_PAL_EXIT ? H2_PAL_ERR_FORMAT : rc;
    p = in->data + in->offset;
    const uint32_t size = le32(p + 4);
    if (!memcmp(p, "data", 4)) {
      if (!have_format)
        return H2_PAL_ERR_FORMAT;
      h2_gizclaw_audio_input_consume(in, 8);
      w->data = in->position;
      w->frames = size == 0u || size == 0xFFFFFFFFu ? UINT64_MAX
                                                   : size / w->block;
      w->state = WAV_READY;
      return H2_PAL_OK;
    }
    if (!memcmp(p, "fmt ", 4)) {
      if (have_format)
        return H2_PAL_ERR_FORMAT;
      const size_t head_len = size < 40u ? size : 40u;
      rc = h2_gizclaw_audio_input_fill(in, 8u + head_len);
      if (rc != H2_PAL_OK)
        return rc == H2_PAL_EXIT ? H2_PAL_ERR_FORMAT : rc;
      rc = parse_format(w, in->data + in->offset + 8, size);
      if (rc != H2_PAL_OK)
        return rc;
      have_format = true;
    }
    rc = h2_gizclaw_audio_input_skip(in, 8u + (uint64_t)size + (size & 1u));
    if (rc != H2_PAL_OK)
      return rc == H2_PAL_EXIT ? H2_PAL_ERR_FORMAT : rc;
  }
}

static int32_t sample_at(const h2_gizclaw_wav_t *w, const uint8_t *p) {
  switch (w->width) {
  case 1:
    return ((int32_t)p[0] - 128) * 256;
  case 2:
    return (int16_t)le16(p);
  case 3:
    return (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
  default:
    break;
  }
  const uint32_t bits = le32(p);
  if (!w->is_float)
    return (int16_t)(bits >> 16);
  float value;
  memcpy(&value, &bits, sizeof(value));
  if (!(value > -1.0f)) /* Also NaN. */
    return value < 0.0f ? -32768 : 0;
  if (value >= 1.0f)
    return 32767;
  return (int32_t)(value * 32767.0f);
}

static h2_pal_result_t finish(h2_gizclaw_wav_t *w, uint64_t *end) {
  w->state = WAV_END;
  *end = w->frame;
  return H2_PAL_EXIT;
}

static h2_pal_result_t step(h2_gizclaw_wav_t *w, size_t max_samples,
                            h2_gizclaw_audio_chunk_t *chunk, uint64_t *end) {
  h2_gizclaw_audio_input_t *in = w->in;
  if (w->state == WAV_HEAD)
    return head(w);
  if (w->state == WAV_END)
    return finish(w, end);
  w->state = WAV_PLAY;
  const uint64_t left = w->frames == UINT64_MAX ? UINT64_MAX
                        : w->frames > w->frame  ? w->frames - w->frame
                                                : 0u;
  if (!left)
    return finish(w, end);
  if (w->frame < w->skip_until) {
    uint64_t count = w->skip_until - w->frame;
    if (count > left)
      count = left;
    if (count > SKIP_BYTES / w->block)
      count = SKIP_BYTES / w->block;
    uint64_t skipped = count * w->block;
    const h2_pal_result_t rc = h2_gizclaw_audio_input_skip(in, skipped);
    if (rc == H2_PAL_EXIT)
      return finish(w, end); /* Ends inside the last frame or before it. */
    if (rc != H2_PAL_OK)
      return rc;
    w->frame += count;
    return H2_PAL_OK;
  }
  size_t count = CHUNK_FRAMES;
  if (count > max_samples)
    count = max_samples;
  if (count > H2_GIZCLAW_AUDIO_INPUT_BYTES / w->block)
    count = H2_GIZCLAW_AUDIO_INPUT_BYTES / w->block;
  if (count > left)
    count = (size_t)left;
  const h2_pal_result_t rc = h2_gizclaw_audio_input_fill(in, count * w->block);
  if (rc == H2_PAL_EXIT)
    count = in->len / w->block; /* A partial last frame is not audio. */
  else if (rc != H2_PAL_OK)
    return rc;
  if (!count)
    return finish(w, end);
  const uint8_t *p = in->data + in->offset;
  for (size_t i = 0; i < count; ++i) {
    int32_t sum = 0;
    for (uint32_t c = 0; c < w->channels; ++c, p += w->width)
      sum += sample_at(w, p);
    w->pcm[i] = (int16_t)(sum / (int32_t)w->channels);
  }
  h2_gizclaw_audio_input_consume(in, count * w->block);
  *chunk = (h2_gizclaw_audio_chunk_t){
      .samples = w->pcm, .count = count, .start = w->frame};
  w->frame += count;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_wav_next(h2_gizclaw_wav_t *w, size_t max_samples,
                                    h2_gizclaw_audio_chunk_t *chunk,
                                    uint64_t *end) {
  if (w == NULL || chunk == NULL || end == NULL || max_samples == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  *chunk = (h2_gizclaw_audio_chunk_t){0};
  if (w->error != H2_PAL_OK)
    return w->error;
  const h2_pal_result_t rc = step(w, max_samples, chunk, end);
  if (rc != H2_PAL_OK && rc != H2_PAL_EXIT)
    w->error = rc;
  return rc;
}

uint32_t h2_gizclaw_wav_rate(const h2_gizclaw_wav_t *w) {
  return w != NULL && w->state != WAV_HEAD ? w->rate : 0u;
}

bool h2_gizclaw_wav_headers_done(const h2_gizclaw_wav_t *w) {
  return w != NULL && w->error == H2_PAL_OK && w->state == WAV_READY;
}

static uint64_t pre_rolled(uint64_t target) {
  return target > PRE_ROLL_FRAMES ? target - PRE_ROLL_FRAMES : 0u;
}

h2_pal_result_t h2_gizclaw_wav_seek_offset(const h2_gizclaw_wav_t *w,
                                           uint64_t target, uint64_t total,
                                           uint64_t *offset) {
  if (w == NULL || offset == NULL || target > UINT64_MAX / 256u)
    return H2_PAL_ERR_INVALID_ARG;
  if (!h2_gizclaw_wav_headers_done(w))
    return H2_PAL_ERR_INVALID_STATE;
  const uint64_t frame = pre_rolled(target);
  if (frame >= w->frames)
    return H2_PAL_ERR_FORMAT;
  *offset = w->data + frame * w->block;
  return *offset < total ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

h2_pal_result_t h2_gizclaw_wav_seek(h2_gizclaw_wav_t *w, uint64_t target,
                                    bool resync) {
  if (w == NULL || target > UINT64_MAX / 256u)
    return H2_PAL_ERR_INVALID_ARG;
  if (!h2_gizclaw_wav_headers_done(w))
    return H2_PAL_ERR_INVALID_STATE;
  if (resync) {
    /* The reader was restarted at an offset seek_offset() produced. */
    const uint64_t position = w->in->position;
    if (position < w->data || (position - w->data) % w->block)
      return H2_PAL_ERR_INVALID_ARG;
    w->frame = (position - w->data) / w->block;
  }
  w->skip_until = pre_rolled(target);
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_wav_create(const h2_pal_mem_api_t *allocator,
                                      h2_gizclaw_audio_input_t *input,
                                      h2_gizclaw_wav_t **out) {
  if (out != NULL)
    *out = NULL;
  if (out == NULL || input == NULL || allocator == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_wav_t *w = h2_pal_mem_alloc(allocator, sizeof(*w));
  if (w == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(w, 0, sizeof(*w));
  w->allocator = allocator;
  w->in = input;
  w->frames = UINT64_MAX;
  *out = w;
  return H2_PAL_OK;
}

void h2_gizclaw_wav_destroy(h2_gizclaw_wav_t *w) {
  if (w != NULL)
    h2_pal_mem_free(w->allocator, w);
}
