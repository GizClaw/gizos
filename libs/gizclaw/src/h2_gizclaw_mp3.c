#include "h2_gizclaw_audio_decoder_internal.h"

#include "dr_mp3.h"

#include <string.h>

/* Unusable bytes tolerated before the first frame or after a ranged
 * landing. Lost sync mid-track scans to the end instead: trailing APE tags
 * can hold cover art of any size. */
#define SCAN_LIMIT 65536u
/* Frames decoded and dropped before a seek target: the bit reservoir reaches
 * back at most 511 bytes (about five frames at the lowest bitrates) and the
 * synthesis overlap needs one more. */
#define PRE_ROLL_FRAMES 8u
/* An offset that is only an estimate aims this far before the target so it
 * lands early, as the Ogg/Opus path does. */
#define SEEK_BACKOFF_MS 5000u
/* LAME counts its delays without the decoder's own 528 + 1 samples. */
#define DECODER_DELAY 529u

typedef struct frame {
  uint32_t len, rate, spf, channels, kbps, side;
} frame_t;

typedef enum mp3_state {
  MP3_HEAD,  /* Tags and the first frame not parsed yet. */
  MP3_READY, /* Headers parsed, no frame read: seeks are accepted. */
  MP3_PLAY,
  MP3_END,
} mp3_state_t;

struct h2_gizclaw_mp3 {
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_audio_input_t *in;
  mp3_state_t state;
  h2_pal_result_t error;
  /* The first frame's header: every later frame must share its version,
   * layer, sample rate and channel count. */
  uint8_t ref[4];
  uint32_t rate, spf, kbps;
  /* File offsets of the tag frame (when there is one) and the first audio
   * frame. */
  uint64_t tag_offset, first;
  /* From a Xing/Info/VBRI frame and its LAME extension. total is the track
   * length in samples, UINT64_MAX when unknown. */
  bool cbr, vbr, toc_ok;
  uint32_t frames, bytes;
  uint8_t toc[100];
  uint32_t delay;
  uint64_t total;
  /* Playback: frame is the index of the next frame (0 = first audio
   * frame); frames before skip_until are parsed, not decoded. */
  uint64_t frame, skip_until;
  bool reset, landing;
  drmp3dec dec;
  int16_t pcm[1152u * 2u];
};

static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool parse(const uint8_t *h, frame_t *f) {
  static const uint16_t kbps1[16] = {0,   32,  40,  48,  56,  64,  80,  96,
                                     112, 128, 160, 192, 224, 256, 320, 0};
  static const uint16_t kbps2[16] = {0,  8,  16, 24,  32,  40,  48,  56,
                                     64, 80, 96, 112, 128, 144, 160, 0};
  static const uint16_t rates[3] = {44100, 48000, 32000};
  if (h[0] != 0xFFu || (h[1] & 0xE0u) != 0xE0u)
    return false;
  const unsigned version = (h[1] >> 3) & 3u; /* 3 MPEG-1, 2 MPEG-2, 0 2.5 */
  const unsigned bitrate = h[2] >> 4, rate = (h[2] >> 2) & 3u;
  if (version == 1u || ((h[1] >> 1) & 3u) != 1u || bitrate == 0u ||
      bitrate == 15u || rate == 3u || (h[3] & 3u) == 2u)
    return false;
  const bool mpeg1 = version == 3u;
  f->rate = (uint32_t)rates[rate] >> (mpeg1 ? 0 : version == 2u ? 1 : 2);
  f->kbps = mpeg1 ? kbps1[bitrate] : kbps2[bitrate];
  f->spf = mpeg1 ? 1152u : 576u;
  f->channels = (h[3] >> 6) == 3u ? 1u : 2u;
  f->len = f->spf / 8u * f->kbps * 1000u / f->rate + ((h[2] >> 1) & 1u);
  f->side = (mpeg1 ? (f->channels == 1u ? 17u : 32u)
                   : (f->channels == 1u ? 9u : 17u)) +
            ((h[1] & 1u) ? 0u : 2u);
  return true;
}

bool h2_gizclaw_mp3_header_valid(const uint8_t header[4]) {
  frame_t f;
  return header != NULL && parse(header, &f);
}

static bool same_stream(const uint8_t *a, const uint8_t *b) {
  return ((a[1] ^ b[1]) & 0xFEu) == 0 && ((a[2] ^ b[2]) & 0x0Cu) == 0 &&
         ((a[3] >> 6) == 3u) == ((b[3] >> 6) == 3u);
}

static const uint8_t *head_at(const h2_gizclaw_mp3_t *m) {
  return m->in->data + m->in->offset;
}

/* Advance to a frame of this stream whose next header, or the end of the
 * stream, confirms it. EXIT when the stream ends first, FORMAT past limit
 * dropped bytes. */
static h2_pal_result_t scan(h2_gizclaw_mp3_t *m, frame_t *f, uint64_t limit) {
  h2_gizclaw_audio_input_t *in = m->in;
  const bool have_ref = m->state != MP3_HEAD;
  for (uint64_t dropped = 0;; ++dropped) {
    if (dropped > limit)
      return H2_PAL_ERR_FORMAT;
    h2_pal_result_t rc = h2_gizclaw_audio_input_fill(in, 4);
    if (rc != H2_PAL_OK)
      return rc;
    if (parse(head_at(m), f) && (!have_ref || same_stream(m->ref, head_at(m)))) {
      rc = h2_gizclaw_audio_input_fill(in, f->len + 4u);
      if (rc != H2_PAL_OK && rc != H2_PAL_EXIT)
        return rc;
      const uint8_t *p = head_at(m);
      frame_t next = {0};
      if (rc == H2_PAL_EXIT ? in->len >= f->len
                            : parse(p + f->len, &next) &&
                                  same_stream(p, p + f->len))
        return H2_PAL_OK;
    }
    h2_gizclaw_audio_input_consume(in, 1);
  }
}

/* The first frame may be metadata: a Xing (VBR) or Info (CBR) frame after
 * the side information, or a VBRI frame 32 bytes in. */
static bool parse_tag(h2_gizclaw_mp3_t *m, const uint8_t *p, const frame_t *f) {
  size_t at = 4u + f->side;
  bool have_frames = false;
  if (f->len >= at + 8u &&
      (!memcmp(p + at, "Xing", 4) || !memcmp(p + at, "Info", 4))) {
    m->cbr = p[at] == 'I';
    m->vbr = !m->cbr;
    const uint32_t flags = be32(p + at + 4u);
    at += 8u;
    if ((flags & 1u) && at + 4u <= f->len) {
      m->frames = be32(p + at);
      have_frames = true;
      at += 4u;
    }
    if ((flags & 2u) && at + 4u <= f->len) {
      m->bytes = be32(p + at);
      at += 4u;
    }
    if ((flags & 4u) && at + 100u <= f->len) {
      memcpy(m->toc, p + at, sizeof(m->toc));
      m->toc_ok = m->vbr;
      for (size_t i = 1; i < sizeof(m->toc); ++i)
        if (m->toc[i] < m->toc[i - 1])
          m->toc_ok = false;
      at += 100u;
    }
    if (flags & 8u)
      at += 4u;
    /* LAME extension: 12-bit encoder delay and padding 21 bytes in. */
    uint32_t padding = 0;
    if (at + 24u <= f->len && p[at] != 0) {
      const uint8_t *lame = p + at + 21u;
      m->delay = (((uint32_t)lame[0] << 4) | ((uint32_t)lame[1] >> 4)) +
                 DECODER_DELAY;
      padding = (((uint32_t)lame[1] & 0xFu) << 8) | (uint32_t)lame[2];
      padding = padding > DECODER_DELAY ? padding - DECODER_DELAY : 0u;
    }
    const uint64_t raw = (uint64_t)m->frames * f->spf;
    if (have_frames && m->frames && raw > (uint64_t)m->delay + padding)
      m->total = raw - m->delay - padding;
    if (m->bytes <= f->len)
      m->toc_ok = false;
    return true;
  }
  if (f->len >= 36u + 18u && !memcmp(p + 36, "VBRI", 4)) {
    m->vbr = true;
    m->bytes = be32(p + 36u + 10u);
    m->frames = be32(p + 36u + 14u);
    if (m->frames)
      m->total = (uint64_t)m->frames * f->spf;
    return true;
  }
  return false;
}

/* ID3v2 tags (any number, any size), then the first frame. */
static h2_pal_result_t head(h2_gizclaw_mp3_t *m) {
  h2_gizclaw_audio_input_t *in = m->in;
  for (;;) {
    h2_pal_result_t rc = h2_gizclaw_audio_input_fill(in, 10);
    if (rc == H2_PAL_EXIT)
      break;
    if (rc != H2_PAL_OK)
      return rc;
    const uint8_t *p = head_at(m);
    if (memcmp(p, "ID3", 3) != 0 || p[3] == 0xFFu || p[4] == 0xFFu ||
        ((p[6] | p[7] | p[8] | p[9]) & 0x80u))
      break;
    uint64_t size = ((uint64_t)p[6] << 21) | ((uint64_t)p[7] << 14) |
                    ((uint64_t)p[8] << 7) | (uint64_t)p[9];
    if (p[5] & 0x10u)
      size += 10u; /* Footer. */
    rc = h2_gizclaw_audio_input_skip(in, 10u + size);
    if (rc == H2_PAL_EXIT)
      return H2_PAL_ERR_FORMAT;
    if (rc != H2_PAL_OK)
      return rc;
  }
  frame_t f = {0};
  h2_pal_result_t rc = scan(m, &f, SCAN_LIMIT);
  if (rc == H2_PAL_EXIT)
    return H2_PAL_ERR_FORMAT; /* No audio frame at all. */
  if (rc != H2_PAL_OK)
    return rc;
  memcpy(m->ref, head_at(m), sizeof(m->ref));
  m->rate = f.rate;
  m->spf = f.spf;
  m->kbps = f.kbps;
  m->tag_offset = in->position;
  if (parse_tag(m, head_at(m), &f)) {
    h2_gizclaw_audio_input_consume(in, f.len);
    /* A CBR estimate uses the first audio frame's own bitrate. */
    frame_t audio = {0};
    rc = h2_gizclaw_audio_input_fill(in, 4);
    if (rc != H2_PAL_OK && rc != H2_PAL_EXIT)
      return rc;
    if (in->len >= 4u && parse(head_at(m), &audio) &&
        same_stream(m->ref, head_at(m)))
      m->kbps = audio.kbps;
  }
  m->first = in->position;
  m->state = MP3_READY;
  return H2_PAL_OK;
}

/* Byte offset of frame `index` and its inverse, by the best model the
 * stream offers: the Xing TOC, the Xing frame and byte counts, or the
 * constant frame size of the first audio frame's bitrate. The TOC spans the
 * tag frame too, so offsets are kept past it. */
static uint64_t offset_of(const h2_gizclaw_mp3_t *m, uint64_t index) {
  if (m->toc_ok && m->frames) {
    const double percent = (double)index * 100.0 / (double)m->frames;
    if (percent >= 100.0)
      return m->tag_offset + m->bytes;
    const unsigned i = (unsigned)percent;
    const double a = m->toc[i], b = i < 99u ? m->toc[i + 1u] : 256.0;
    const double at = a + (b - a) * (percent - (double)i);
    const uint64_t offset =
        m->tag_offset + (uint64_t)(at * (double)m->bytes / 256.0);
    return offset > m->first ? offset : m->first;
  }
  const uint64_t tag = m->first - m->tag_offset;
  if (m->vbr && m->frames && m->bytes > tag)
    return m->first + index * (m->bytes - tag) / m->frames;
  return m->first +
         index * (uint64_t)(m->spf / 8u) * m->kbps * 1000u / m->rate;
}

static uint64_t frame_at(const h2_gizclaw_mp3_t *m, uint64_t offset) {
  if (offset <= m->first)
    return 0;
  if (m->toc_ok && m->frames) {
    const double at = offset > m->tag_offset
                          ? (double)(offset - m->tag_offset) * 256.0 /
                                (double)m->bytes
                          : 0.0;
    unsigned i = 99u;
    while (i > 0u && m->toc[i] > at)
      --i;
    const double a = m->toc[i], b = i < 99u ? m->toc[i + 1u] : 256.0;
    double percent = (double)i + (b > a ? (at - a) / (b - a) : 0.0);
    if (percent > 100.0)
      percent = 100.0;
    return (uint64_t)(percent * (double)m->frames / 100.0 + 0.5);
  }
  const uint64_t into = offset > m->first ? offset - m->first : 0u;
  const uint64_t tag = m->first - m->tag_offset;
  if (m->vbr && m->frames && m->bytes > tag)
    return (into * m->frames + (m->bytes - tag) / 2u) / (m->bytes - tag);
  const uint64_t per = (uint64_t)(m->spf / 8u) * m->kbps * 1000u;
  return (into * m->rate + per / 2u) / per;
}

static h2_pal_result_t finish(h2_gizclaw_mp3_t *m, uint64_t *end) {
  const uint64_t raw = m->frame * m->spf;
  uint64_t length = raw > m->delay ? raw - m->delay : 0u;
  if (length > m->total)
    length = m->total;
  m->state = MP3_END;
  *end = length;
  return H2_PAL_EXIT;
}

static h2_pal_result_t step(h2_gizclaw_mp3_t *m, h2_gizclaw_audio_chunk_t *chunk,
                            uint64_t *end) {
  h2_gizclaw_audio_input_t *in = m->in;
  if (m->state == MP3_HEAD)
    return head(m);
  if (m->state == MP3_END)
    return finish(m, end);
  m->state = MP3_PLAY;
  frame_t f = {0};
  h2_pal_result_t rc;
  if (m->landing) {
    if (in->position < m->first) {
      rc = h2_gizclaw_audio_input_skip(in, m->first - in->position);
      if (rc != H2_PAL_OK)
        return rc == H2_PAL_EXIT ? H2_PAL_ERR_FORMAT : rc;
    }
    rc = scan(m, &f, SCAN_LIMIT);
    if (rc == H2_PAL_EXIT)
      return H2_PAL_ERR_FORMAT; /* Nothing to land on before the end. */
    if (rc != H2_PAL_OK)
      return rc;
    m->frame = frame_at(m, in->position);
    m->landing = false;
    m->reset = true;
  } else {
    rc = h2_gizclaw_audio_input_fill(in, 4);
    if (rc == H2_PAL_EXIT)
      return finish(m, end);
    if (rc != H2_PAL_OK)
      return rc;
    if (!parse(head_at(m), &f) || !same_stream(m->ref, head_at(m))) {
      /* Lost sync: the next confirmed frame goes on, or the track ends. */
      rc = scan(m, &f, UINT64_MAX);
      if (rc == H2_PAL_EXIT)
        return finish(m, end);
      if (rc != H2_PAL_OK)
        return rc;
      m->reset = true;
    }
  }
  rc = h2_gizclaw_audio_input_fill(in, f.len);
  if (rc == H2_PAL_EXIT)
    return finish(m, end); /* A partial last frame is not audio. */
  if (rc != H2_PAL_OK)
    return rc;
  const uint64_t index = m->frame++;
  if (index < m->skip_until) {
    h2_gizclaw_audio_input_consume(in, f.len);
    m->reset = true;
    return H2_PAL_OK;
  }
  if (m->reset) {
    drmp3dec_init(&m->dec);
    m->reset = false;
  }
  drmp3dec_frame_info info = {0};
  const int decoded =
      drmp3dec_decode_frame(&m->dec, head_at(m), (int)f.len, m->pcm, &info);
  h2_gizclaw_audio_input_consume(in, f.len);
  /* A frame whose reservoir bytes are gone (just after a reset) or whose
   * side information is corrupt decodes to nothing; it still occupies its
   * place on the timeline, as silence. */
  if (decoded == (int)f.spf && info.channels == 2) {
    for (uint32_t i = 0; i < f.spf; ++i)
      m->pcm[i] = (int16_t)(((int32_t)m->pcm[2u * i] + m->pcm[2u * i + 1u]) / 2);
  } else if (decoded != (int)f.spf || info.channels != 1) {
    memset(m->pcm, 0, f.spf * sizeof(m->pcm[0]));
  }
  /* Raw samples [raw, raw + spf) are [raw - delay, ...) on the timeline;
   * the part before 0 is the delay and the part past total the padding. */
  const uint64_t raw = index * f.spf;
  if (raw + f.spf <= m->delay)
    return H2_PAL_OK;
  const uint64_t lead = raw < m->delay ? m->delay - raw : 0u;
  const uint64_t start = raw + lead - m->delay;
  uint64_t count = f.spf - lead;
  if (start >= m->total)
    return H2_PAL_OK;
  if (count > m->total - start)
    count = m->total - start;
  *chunk = (h2_gizclaw_audio_chunk_t){
      .samples = m->pcm + lead, .count = (size_t)count, .start = start};
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_mp3_next(h2_gizclaw_mp3_t *m,
                                    h2_gizclaw_audio_chunk_t *chunk,
                                    uint64_t *end) {
  if (m == NULL || chunk == NULL || end == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *chunk = (h2_gizclaw_audio_chunk_t){0};
  if (m->error != H2_PAL_OK)
    return m->error;
  const h2_pal_result_t rc = step(m, chunk, end);
  if (rc != H2_PAL_OK && rc != H2_PAL_EXIT)
    m->error = rc;
  return rc;
}

uint32_t h2_gizclaw_mp3_rate(const h2_gizclaw_mp3_t *m) {
  return m != NULL && m->state != MP3_HEAD ? m->rate : 0u;
}

bool h2_gizclaw_mp3_headers_done(const h2_gizclaw_mp3_t *m) {
  return m != NULL && m->error == H2_PAL_OK && m->state == MP3_READY;
}

h2_pal_result_t h2_gizclaw_mp3_seek_offset(const h2_gizclaw_mp3_t *m,
                                           uint64_t target, uint64_t total,
                                           uint64_t *offset) {
  if (m == NULL || offset == NULL || target > UINT64_MAX / 2u)
    return H2_PAL_ERR_INVALID_ARG;
  if (!h2_gizclaw_mp3_headers_done(m))
    return H2_PAL_ERR_INVALID_STATE;
  uint64_t index = (target + m->delay) / m->spf;
  uint64_t back = PRE_ROLL_FRAMES;
  if (!m->cbr)
    back += (uint64_t)SEEK_BACKOFF_MS * m->rate / 1000u / m->spf;
  index = index > back ? index - back : 0u;
  *offset = offset_of(m, index);
  return *offset < total ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

h2_pal_result_t h2_gizclaw_mp3_seek(h2_gizclaw_mp3_t *m, uint64_t target,
                                    bool resync) {
  if (m == NULL || target > UINT64_MAX / 2u)
    return H2_PAL_ERR_INVALID_ARG;
  if (!h2_gizclaw_mp3_headers_done(m))
    return H2_PAL_ERR_INVALID_STATE;
  const uint64_t index = (target + m->delay) / m->spf;
  m->skip_until = index > PRE_ROLL_FRAMES ? index - PRE_ROLL_FRAMES : 0u;
  m->landing = resync;
  m->reset = true;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_mp3_create(const h2_pal_mem_api_t *allocator,
                                      h2_gizclaw_audio_input_t *input,
                                      h2_gizclaw_mp3_t **out) {
  if (out != NULL)
    *out = NULL;
  if (out == NULL || input == NULL || allocator == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_mp3_t *m = h2_pal_mem_alloc(allocator, sizeof(*m));
  if (m == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(m, 0, sizeof(*m));
  m->allocator = allocator;
  m->in = input;
  m->total = UINT64_MAX;
  m->reset = true;
  *out = m;
  return H2_PAL_OK;
}

void h2_gizclaw_mp3_destroy(h2_gizclaw_mp3_t *m) {
  if (m != NULL)
    h2_pal_mem_free(m->allocator, m);
}
