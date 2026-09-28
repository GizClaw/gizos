#include "h2_gizclaw_audio_decoder_internal.h"
#include "h2_gizclaw_resample_internal.h"
#include "ogg_opus_fixture.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
#define NO_SEEK UINT64_MAX

/* Allocator that tracks live and peak bytes so every decode also proves the
 * decoder frees what it takes. */
static size_t s_live, s_peak;
static void *allocate(void *user, size_t len) {
  (void)user;
  size_t *p = malloc(len + sizeof(size_t) * 2u);
  assert(p != NULL);
  p[0] = len;
  s_live += len;
  if (s_live > s_peak)
    s_peak = s_live;
  return p + 2;
}
static void release(void *user, void *ptr) {
  (void)user;
  if (ptr == NULL)
    return;
  size_t *p = (size_t *)ptr - 2;
  s_live -= p[0];
  free(p);
}
static const h2_pal_mem_vtable_t s_mem_vtable = {.alloc = allocate,
                                                 .free = release};
static const h2_pal_mem_api_t s_mem = {.vtable = &s_mem_vtable};

/* A file served from memory at most `chunk` bytes per read. */
typedef struct source {
  const uint8_t *data;
  size_t len, pos, chunk;
} source_t;
static h2_pal_result_t source_read(void *user, uint8_t *out, size_t capacity,
                                   size_t *out_len) {
  source_t *s = user;
  if (s->pos >= s->len)
    return H2_PAL_EXIT;
  size_t n = s->len - s->pos;
  if (n > capacity)
    n = capacity;
  if (s->chunk && n > s->chunk)
    n = s->chunk;
  memcpy(out, s->data + s->pos, n);
  s->pos += n;
  *out_len = n;
  return H2_PAL_OK;
}

typedef struct decoded {
  int16_t *samples;
  size_t count, cap;
  h2_pal_result_t rc;
  bool located;
  uint64_t origin;
  size_t peak;
} decoded_t;

static void append(decoded_t *out, const uint8_t *pcm, size_t len) {
  assert(len % 2u == 0);
  if (out->count + len / 2u > out->cap) {
    out->cap = (out->count + len / 2u) * 2u + 4096u;
    out->samples = realloc(out->samples, out->cap * sizeof(int16_t));
    assert(out->samples != NULL);
  }
  for (size_t i = 0; i < len; i += 2u)
    out->samples[out->count++] =
        (int16_t)(uint16_t)(pcm[i] | (uint16_t)(pcm[i + 1u] << 8));
}

/* Decode to the end, seeking to seek_ms once the headers are parsed. With
 * resync the source first moves to the decoder's own seek offset, the way
 * the player's ranged request does. */
static decoded_t decode_ex(const uint8_t *data, size_t len, size_t chunk,
                           uint64_t seek_ms, bool resync,
                           uint64_t duration_ms) {
  source_t src = {.data = data, .len = len, .chunk = chunk};
  h2_gizclaw_audio_decoder_t *d = NULL;
  s_peak = 0;
  assert(h2_gizclaw_audio_decoder_create(&s_mem, source_read, &src, &d) ==
         H2_PAL_OK);
  decoded_t out = {0};
  bool pending = seek_ms != NO_SEEK;
  uint8_t pcm[H2_GIZCLAW_AUDIO_PCM_BYTES];
  for (;;) {
    size_t n = 0;
    const h2_pal_result_t rc =
        h2_gizclaw_audio_decoder_next(d, pcm, sizeof(pcm), &n);
    assert(n <= sizeof(pcm));
    append(&out, pcm, n);
    if (rc == H2_PAL_OK && pending &&
        h2_gizclaw_audio_decoder_headers_done(d)) {
      pending = false;
      uint64_t offset = 0;
      if (resync) {
        assert(h2_gizclaw_audio_decoder_seek_offset(d, seek_ms, duration_ms,
                                                    len, &offset) == H2_PAL_OK);
        assert(offset < len);
        src.pos = (size_t)offset;
      }
      assert(h2_gizclaw_audio_decoder_seek(d, seek_ms, resync, offset) ==
             H2_PAL_OK);
    }
    if (!out.located && !pending &&
        h2_gizclaw_audio_decoder_origin(d, &out.origin)) {
      out.located = true;
      /* A seek emits nothing before the sample its origin names. */
      assert(seek_ms == NO_SEEK || out.count == n / 2u);
    }
    if (rc != H2_PAL_OK) {
      out.rc = rc;
      break;
    }
  }
  out.peak = s_peak;
  h2_gizclaw_audio_decoder_destroy(d);
  assert(s_live == 0);
  return out;
}

static decoded_t decode(const uint8_t *data, size_t len) {
  return decode_ex(data, len, 4096u, NO_SEEK, false, 0);
}

static h2_pal_result_t decode_rc(const uint8_t *data, size_t len) {
  decoded_t out = decode(data, len);
  free(out.samples);
  return out.rc;
}

/* Share of the signal's energy at `freq` Hz (Goertzel), 1.0 for a pure
 * tone over whole periods. */
static double tone_share(const int16_t *x, size_t n, double freq) {
  const double c = 2.0 * cos(2.0 * PI * freq / 16000.0);
  double s1 = 0.0, s2 = 0.0, total = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double s0 = (double)x[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
    total += (double)x[i] * (double)x[i];
  }
  const double power = s1 * s1 + s2 * s2 - c * s1 * s2;
  return total > 0.0 ? 2.0 * power / ((double)n * total) : 0.0;
}

static double rms(const int16_t *x, size_t n) {
  double total = 0.0;
  for (size_t i = 0; i < n; ++i)
    total += (double)x[i] * (double)x[i];
  return n ? sqrt(total / (double)n) : 0.0;
}

static uint32_t s_rand = 12345u;
static uint32_t next_rand(void) {
  s_rand = s_rand * 1103515245u + 12345u;
  return s_rand >> 8;
}

/* --- Resampler --------------------------------------------------------- */

static const uint32_t s_rates[] = {8000,  11025, 12000, 16000, 22050,
                                   24000, 32000, 44100, 48000};

/* Run `count` samples of fn through a resampler that starts at source
 * sample s0, in pushes of random size, and return the outputs. */
static size_t resample_run(uint32_t rate, uint64_t s0, size_t count,
                           double (*fn)(uint32_t rate, size_t i),
                           int16_t *out) {
  h2_gizclaw_resample_t r;
  const uint64_t first16 = (s0 * 16000u + rate - 1u) / rate;
  assert(h2_gizclaw_resample_reset(&r, rate,
                                   (uint32_t)(first16 * rate - s0 * 16000u)));
  int16_t in[H2_GIZCLAW_RESAMPLE_INPUT_MAX];
  size_t produced = 0;
  for (size_t done = 0; done < count;) {
    size_t n = 1u + next_rand() % H2_GIZCLAW_RESAMPLE_INPUT_MAX;
    if (n > count - done)
      n = count - done;
    for (size_t i = 0; i < n; ++i)
      in[i] = (int16_t)lround(fn(rate, done + i));
    const size_t got = h2_gizclaw_resample_push(&r, in, n, out + produced);
    assert(got <= h2_gizclaw_resample_capacity(&r, n));
    produced += got;
    done += n;
  }
  const size_t tail = h2_gizclaw_resample_flush(&r, out + produced);
  assert(tail <= h2_gizclaw_resample_capacity(&r, 0));
  assert(first16 + produced + tail ==
         ((s0 + count) * 16000u + rate - 1u) / rate);
  return produced + tail;
}

static double dc(uint32_t rate, size_t i) {
  (void)rate;
  (void)i;
  return 10000.0;
}
static double tone_1k(uint32_t rate, size_t i) {
  return 10000.0 * sin(2.0 * PI * 1000.0 * (double)i / (double)rate);
}
static double tone_9k(uint32_t rate, size_t i) {
  return 10000.0 * sin(2.0 * PI * 9000.0 * (double)i / (double)rate);
}
static double tone_3k(uint32_t rate, size_t i) {
  return 10000.0 * sin(2.0 * PI * 3000.0 * (double)i / (double)rate);
}

static void test_resample(void) {
  static int16_t out[40000];
  h2_gizclaw_resample_t r;
  assert(!h2_gizclaw_resample_reset(&r, 7999, 0));
  assert(!h2_gizclaw_resample_reset(&r, 48001, 0));
  assert(!h2_gizclaw_resample_reset(&r, 44100, 44100));
  assert(!h2_gizclaw_resample_reset(&r, 16000, 1));
  for (size_t k = 0; k < sizeof(s_rates) / sizeof(s_rates[0]); ++k) {
    const uint32_t rate = s_rates[k];
    const uint64_t starts[] = {0, 1, 441, 12345};
    for (size_t s = 0; s < 4u; ++s) {
      /* Exact output counts on the shared grid; unity gain at DC away from
       * the zero-padded edges. */
      const size_t n = resample_run(rate, starts[s], rate * 3u / 2u, dc, out);
      for (size_t i = 40; i + 40 < n; ++i)
        assert(out[i] >= 9999 && out[i] <= 10001);
    }
    /* The passband keeps a 1 kHz tone to within 0.1 dB. */
    size_t n = resample_run(rate, 0, rate, tone_1k, out);
    assert(n == 16000u);
    const double level = rms(out + 1000, 14000) / (10000.0 / sqrt(2.0));
    assert(level > 0.988 && level < 1.012);
    assert(tone_share(out + 1000, 14000, 1000.0) > 0.999);
    if (rate > 16000u) {
      /* 9 kHz cannot be represented at 16 kHz: it must not fold down. */
      n = resample_run(rate, 0, rate, tone_9k, out);
      assert(rms(out + 1000, 14000) < 10000.0 * 1.8e-3);
    } else if (rate < 16000u) {
      /* Interpolation must not leave the 3 kHz tone's image above the
       * source Nyquist. */
      n = resample_run(rate, 0, rate, tone_3k, out);
      const double image = (double)rate - 3000.0;
      assert(tone_share(out + 1000, 14000, image) < 1e-5);
      assert(tone_share(out + 1000, 14000, 3000.0) > 0.99);
    }
  }
  /* 16 kHz is a pass-through. */
  int16_t in[300], copy[300];
  for (size_t i = 0; i < 300; ++i)
    in[i] = (int16_t)(next_rand() & 0xFFFF);
  assert(h2_gizclaw_resample_reset(&r, 16000, 0));
  assert(h2_gizclaw_resample_push(&r, in, 300, copy) == 300);
  assert(!memcmp(in, copy, sizeof(in)));
  assert(h2_gizclaw_resample_flush(&r, copy) == 0);
}

/* --- WAV ---------------------------------------------------------------- */

static void put16(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

typedef struct wav_spec {
  uint16_t tag, channels, bits;
  uint32_t rate;
  bool extensible;
  uint16_t sub_tag;
  uint32_t data_size;     /* 0: the payload length. */
  uint16_t block;         /* 0: channels * bits / 8. */
  bool list_before;       /* Odd-sized chunk before "fmt ". */
  bool list_after;        /* Chunk after "data". */
  bool data_before_format;
} wav_spec_t;

static size_t build_wav(uint8_t *out, const wav_spec_t *spec,
                        const uint8_t *payload, size_t payload_len) {
  size_t n = 12;
  memcpy(out, "RIFF", 4);
  memcpy(out + 8, "WAVE", 4);
  if (spec->list_before) {
    memcpy(out + n, "LIST", 4);
    put32(out + n + 4, 3);
    memcpy(out + n + 8, "abc", 3);
    out[n + 11] = 0; /* Pad byte. */
    n += 12;
  }
  const size_t format_at = n;
  if (spec->data_before_format)
    n += 8; /* Leave room for an empty "data" first. */
  const uint32_t format_len = spec->extensible ? 40u : 16u;
  memcpy(out + n, "fmt ", 4);
  put32(out + n + 4, format_len);
  uint8_t *f = out + n + 8;
  memset(f, 0, format_len);
  put16(f, spec->extensible ? 0xFFFEu : spec->tag);
  put16(f + 2, spec->channels);
  put32(f + 4, spec->rate);
  const uint32_t block =
      spec->block ? spec->block : spec->channels * (spec->bits / 8u);
  put32(f + 8, spec->rate * block);
  put16(f + 12, block);
  put16(f + 14, spec->bits);
  if (spec->extensible) {
    static const uint8_t tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10,
                                     0x00, 0x80, 0x00, 0x00, 0xAA,
                                     0x00, 0x38, 0x9B, 0x71};
    put16(f + 16, 22);
    put16(f + 18, spec->bits);
    put16(f + 24, spec->sub_tag ? spec->sub_tag : spec->tag);
    memcpy(f + 26, tail, sizeof(tail));
  }
  n += 8u + format_len;
  if (spec->data_before_format) {
    memcpy(out + format_at, "data", 4);
    put32(out + format_at + 4, 0);
  }
  memcpy(out + n, "data", 4);
  put32(out + n + 4,
        spec->data_size ? spec->data_size : (uint32_t)payload_len);
  memcpy(out + n + 8, payload, payload_len);
  n += 8u + payload_len;
  if (spec->list_after) {
    memcpy(out + n, "LIST", 4);
    put32(out + n + 4, 8);
    memset(out + n + 8, 0x7F, 8);
    n += 16;
  }
  put32(out + 4, (uint32_t)(n - 8u));
  return n;
}

static void test_wav_encodings(void) {
  static uint8_t file[65536], payload[32768];
  /* 16 kHz mono PCM16 passes through bit-exact. */
  for (size_t i = 0; i < 3000; ++i)
    put16(payload + 2u * i, next_rand() & 0xFFFFu);
  wav_spec_t spec = {.tag = 1, .channels = 1, .bits = 16, .rate = 16000,
                     .list_before = true, .list_after = true};
  size_t len = build_wav(file, &spec, payload, 6000);
  decoded_t out = decode(file, len);
  assert(out.rc == H2_PAL_EXIT && out.count == 3000 && out.located &&
         out.origin == 0);
  for (size_t i = 0; i < 3000; ++i)
    assert((uint16_t)out.samples[i] ==
           (uint16_t)(payload[2u * i] | (payload[2u * i + 1u] << 8)));
  free(out.samples);

  /* One constant frame per encoding, mixed down to mono. */
  const struct {
    wav_spec_t spec;
    uint8_t frame[32];
    size_t frame_len;
    int16_t expected;
  } cases[] = {
      {{.tag = 1, .channels = 2, .bits = 16},
       {0xE8, 0x03, 0xB8, 0x0B}, 4, 2000},
      {{.tag = 1, .channels = 1, .bits = 8}, {0xC0}, 1, 16384},
      {{.tag = 1, .channels = 1, .bits = 24}, {0x56, 0x34, 0x12}, 3, 0x1234},
      {{.tag = 1, .channels = 1, .bits = 32}, {0x00, 0x00, 0xFF, 0x7F}, 4,
       32767},
      {{.tag = 3, .channels = 1, .bits = 32}, {0x00, 0x00, 0x00, 0x3F}, 4,
       16383},
      {{.tag = 3, .channels = 1, .bits = 32}, {0x00, 0x00, 0x00, 0xC0}, 4,
       -32768},
      {{.tag = 3, .channels = 1, .bits = 32}, {0x00, 0x00, 0xC0, 0x7F}, 4, 0},
      {{.tag = 3, .channels = 1, .bits = 32, .extensible = true},
       {0x00, 0x00, 0x00, 0xBF}, 4, -16383},
      {{.tag = 1, .channels = 4, .bits = 16},
       {0xE8, 0x03, 0xD0, 0x07, 0xB8, 0x0B, 0xA0, 0x0F}, 8, 2500},
  };
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
    wav_spec_t s = cases[c].spec;
    s.rate = 16000;
    for (size_t i = 0; i < 1000; ++i)
      memcpy(payload + i * cases[c].frame_len, cases[c].frame,
             cases[c].frame_len);
    len = build_wav(file, &s, payload, 1000u * cases[c].frame_len);
    out = decode(file, len);
    assert(out.rc == H2_PAL_EXIT && out.count == 1000);
    for (size_t i = 0; i < 1000; ++i)
      assert(out.samples[i] == cases[c].expected);
    free(out.samples);
  }

  /* An unknown data length plays to the end; a partial last frame is not
   * audio. */
  spec = (wav_spec_t){.tag = 1, .channels = 2, .bits = 16, .rate = 16000,
                      .data_size = 0xFFFFFFFFu};
  memset(payload, 0x11, 4001);
  len = build_wav(file, &spec, payload, 4001);
  out = decode(file, len);
  assert(out.rc == H2_PAL_EXIT && out.count == 1000);
  free(out.samples);

  /* Supported container, unsupported encoding. */
  const wav_spec_t unsupported[] = {
      {.tag = 2, .channels = 1, .bits = 4, .rate = 16000, .block = 1},
      {.tag = 1, .channels = 9, .bits = 16, .rate = 16000},
      {.tag = 1, .channels = 1, .bits = 16, .rate = 96000},
      {.tag = 1, .channels = 1, .bits = 16, .rate = 7999},
      {.tag = 1, .channels = 1, .bits = 12, .rate = 16000, .block = 2},
      {.tag = 3, .channels = 1, .bits = 64, .rate = 16000},
      {.tag = 1, .channels = 1, .bits = 16, .rate = 16000, .extensible = true,
       .sub_tag = 2},
  };
  for (size_t c = 0; c < sizeof(unsupported) / sizeof(unsupported[0]); ++c) {
    len = build_wav(file, &unsupported[c], payload, 64);
    assert(decode_rc(file, len) == H2_PAL_ERR_UNSUPPORTED);
  }
  /* Malformed containers. */
  spec = (wav_spec_t){.tag = 1, .channels = 2, .bits = 16, .rate = 16000,
                      .block = 3};
  len = build_wav(file, &spec, payload, 64);
  assert(decode_rc(file, len) == H2_PAL_ERR_FORMAT);
  spec = (wav_spec_t){.tag = 1, .channels = 1, .bits = 16, .rate = 16000,
                      .data_before_format = true};
  len = build_wav(file, &spec, payload, 64);
  assert(decode_rc(file, len) == H2_PAL_ERR_FORMAT);
  spec = (wav_spec_t){.tag = 1, .channels = 1, .bits = 16, .rate = 16000};
  len = build_wav(file, &spec, payload, 64);
  for (size_t cut = 12; cut < len - 64u; ++cut)
    assert(decode_rc(file, cut) == H2_PAL_ERR_FORMAT);
  memcpy(file + 8, "AVI ", 4);
  assert(decode_rc(file, len) == H2_PAL_ERR_UNSUPPORTED);
}

static void test_wav_resample_and_seek(void) {
  /* Three seconds of a 1 kHz tone at 44.1 kHz stereo, right channel
   * silent: 48000 output samples at half amplitude. */
  const size_t frames = 44100u * 3u;
  uint8_t *payload = malloc(frames * 4u);
  uint8_t *file = malloc(frames * 4u + 64u);
  assert(payload && file);
  for (size_t i = 0; i < frames; ++i) {
    const double v = 16000.0 * sin(2.0 * PI * 1000.0 * (double)i / 44100.0);
    put16(payload + 4u * i, (uint32_t)(int32_t)lround(v) & 0xFFFFu);
    put16(payload + 4u * i + 2u, 0);
  }
  const wav_spec_t spec = {.tag = 1, .channels = 2, .bits = 16,
                           .rate = 44100};
  const size_t len = build_wav(file, &spec, payload, frames * 4u);
  decoded_t full = decode(file, len);
  assert(full.rc == H2_PAL_EXIT && full.count == 48000);
  assert(tone_share(full.samples + 1000, 46000, 1000.0) > 0.999);
  const double level = rms(full.samples + 1000, 46000) / (8000.0 / sqrt(2.0));
  assert(level > 0.99 && level < 1.01);
  printf("wav 44.1k stereo peak memory %zu\n", full.peak);
  assert(full.peak < 20000u);
  /* Buffering does not change a sample. */
  const size_t chunks[] = {1, 7, 4095};
  for (size_t c = 0; c < 3u; ++c) {
    decoded_t other = decode_ex(file, len, chunks[c], NO_SEEK, false, 0);
    assert(other.count == full.count &&
           !memcmp(other.samples, full.samples, full.count * 2u));
    free(other.samples);
  }
  /* Seeking, by skipping or by a ranged restart, is exact: the same
   * samples a full decode has from the target on. */
  const uint64_t targets[] = {1, 1000, 2999};
  for (size_t t = 0; t < 3u; ++t) {
    for (int resync = 0; resync < 2; ++resync) {
      decoded_t part =
          decode_ex(file, len, 4096u, targets[t], resync != 0, 3000);
      assert(part.rc == H2_PAL_EXIT && part.located);
      assert(part.origin == targets[t] * 16u);
      assert(part.count == full.count - part.origin);
      assert(!memcmp(part.samples, full.samples + part.origin,
                     part.count * 2u));
      free(part.samples);
    }
  }
  /* A start past the end: no range, and a skip ends the track at its
   * real end with nothing played. */
  decoded_t past = decode_ex(file, len, 4096u, 3100, false, 3000);
  assert(past.rc == H2_PAL_EXIT && past.count == 0 && past.located &&
         past.origin == 48000);
  free(past.samples);
  source_t src = {.data = file, .len = len};
  h2_gizclaw_audio_decoder_t *d = NULL;
  assert(h2_gizclaw_audio_decoder_create(&s_mem, source_read, &src, &d) ==
         H2_PAL_OK);
  uint64_t offset = 0;
  assert(h2_gizclaw_audio_decoder_seek_offset(d, 1000, 3000, len, &offset) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_audio_decoder_seek(d, 1000, false, 0) ==
         H2_PAL_ERR_INVALID_STATE);
  uint8_t pcm[H2_GIZCLAW_AUDIO_PCM_BYTES];
  size_t n = 0;
  assert(h2_gizclaw_audio_decoder_next(d, pcm, sizeof(pcm), &n) == H2_PAL_OK);
  assert(n == 0 && h2_gizclaw_audio_decoder_headers_done(d));
  assert(h2_gizclaw_audio_decoder_seek_offset(d, 3100, 3000, len, &offset) ==
         H2_PAL_ERR_FORMAT);
  assert(h2_gizclaw_audio_decoder_seek_offset(d, 1000, 3000, 100, &offset) ==
         H2_PAL_ERR_FORMAT);
  assert(h2_gizclaw_audio_decoder_next(d, pcm, sizeof(pcm), &n) == H2_PAL_OK);
  assert(!h2_gizclaw_audio_decoder_headers_done(d));
  assert(h2_gizclaw_audio_decoder_seek(d, 1000, false, 0) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_gizclaw_audio_decoder_next(d, pcm, 100, &n) ==
         H2_PAL_ERR_INVALID_ARG);
  h2_gizclaw_audio_decoder_destroy(d);
  assert(s_live == 0);
  free(full.samples);
  free(payload);
  free(file);
}

/* --- MP3 ---------------------------------------------------------------- */

typedef struct blob {
  uint8_t *data;
  size_t len;
} blob_t;

static blob_t load(const char *path) {
  FILE *file = fopen(path, "rb");
  assert(file != NULL);
  assert(fseek(file, 0, SEEK_END) == 0);
  const long size = ftell(file);
  assert(size > 0);
  assert(fseek(file, 0, SEEK_SET) == 0);
  blob_t blob = {.data = malloc((size_t)size), .len = (size_t)size};
  assert(blob.data && fread(blob.data, 1, blob.len, file) == blob.len);
  fclose(file);
  return blob;
}

/* Byte length of the leading ID3v2 tag, 0 without one. */
static size_t id3_len(const blob_t *b) {
  if (b->len < 10 || memcmp(b->data, "ID3", 3))
    return 0;
  return 10u + (((size_t)b->data[6] << 21) | ((size_t)b->data[7] << 14) |
                ((size_t)b->data[8] << 7) | b->data[9]);
}

/* An ID3v2 tag of `size` body bytes (plus a footer when asked). */
static size_t make_id3(uint8_t *out, size_t size, bool footer) {
  memcpy(out, "ID3", 3);
  out[3] = footer ? 4 : 3;
  out[4] = 0;
  out[5] = footer ? 0x10 : 0;
  out[6] = (uint8_t)((size >> 21) & 0x7F);
  out[7] = (uint8_t)((size >> 14) & 0x7F);
  out[8] = (uint8_t)((size >> 7) & 0x7F);
  out[9] = (uint8_t)(size & 0x7F);
  memset(out + 10, 0xFF, size); /* Sync-like bytes must not confuse it. */
  if (footer) {
    memcpy(out + 10 + size, "3DI", 3);
    memcpy(out + 13 + size, out + 3, 7);
  }
  return 10u + size + (footer ? 10u : 0u);
}

static void assert_same(const decoded_t *a, const decoded_t *b) {
  assert(a->rc == b->rc && a->count == b->count);
  assert(!memcmp(a->samples, b->samples, a->count * 2u));
}

static void test_mp3_cbr(const blob_t *mp3) {
  /* 2 s at 44.1 kHz with a LAME Info tag: delay and padding trimmed, the
   * track is exactly 32000 samples of a half-amplitude 1 kHz tone. */
  decoded_t full = decode(mp3->data, mp3->len);
  assert(full.rc == H2_PAL_EXIT && full.count == 32000);
  assert(tone_share(full.samples + 2000, 28000, 1000.0) > 0.98);
  const double level = rms(full.samples + 2000, 28000) / (8192.0 / sqrt(2.0));
  printf("mp3 cbr level %.3f peak memory %zu\n", level, full.peak);
  assert(level > 0.9 && level < 1.1);
  assert(full.peak < 48000u);
  const size_t chunks[] = {1, 13, 4095};
  for (size_t c = 0; c < 3u; ++c) {
    decoded_t other = decode_ex(mp3->data, mp3->len, chunks[c], NO_SEEK,
                                false, 0);
    assert_same(&other, &full);
    free(other.samples);
  }
  /* A large ID3v2 tag (cover art) streams past, with or without a footer,
   * and so does an ID3v2 tag in front of the first. */
  const size_t tag = id3_len(mp3);
  assert(tag > 0);
  for (int footer = 0; footer < 2; ++footer) {
    uint8_t *big = malloc(mp3->len + 200000u);
    assert(big);
    size_t n = make_id3(big, 150000, footer != 0);
    memcpy(big + n, mp3->data, mp3->len);
    decoded_t tagged = decode(big, n + mp3->len);
    assert_same(&tagged, &full);
    free(tagged.samples);
    free(big);
  }
  /* Seeking by skipping frames and by a ranged restart: CBR with an Info
   * tag places the target exactly, and the samples after the pre-roll are
   * the ones a full decode has. */
  const uint64_t targets[] = {1, 500, 1234, 1990};
  for (size_t t = 0; t < 4u; ++t) {
    for (int resync = 0; resync < 2; ++resync) {
      decoded_t part =
          decode_ex(mp3->data, mp3->len, 4096u, targets[t], resync != 0, 2000);
      assert(part.rc == H2_PAL_EXIT && part.located);
      assert(part.origin == targets[t] * 16u);
      assert(part.count == full.count - part.origin);
      int worst = 0;
      for (size_t i = 0; i < part.count; ++i) {
        const int diff = abs(part.samples[i] - full.samples[part.origin + i]);
        worst = diff > worst ? diff : worst;
      }
      assert(worst == 0);
      free(part.samples);
    }
  }
  /* Past the end: the skip ends the track at its real length. */
  decoded_t past = decode_ex(mp3->data, mp3->len, 4096u, 2500, false, 2000);
  assert(past.rc == H2_PAL_EXIT && past.count == 0 && past.origin == 32000);
  free(past.samples);
  /* Every truncation ends cleanly: an incomplete tag or no whole frame is
   * FORMAT, anything else is a shorter track. */
  for (size_t cut = 0; cut < mp3->len; cut += 211u) {
    decoded_t part = decode(mp3->data, cut);
    if (cut <= tag + 4u)
      assert(part.rc == H2_PAL_ERR_FORMAT || (cut == 0 && part.count == 0));
    else
      assert(part.rc == H2_PAL_EXIT || part.rc == H2_PAL_ERR_FORMAT);
    assert(part.count <= full.count);
    free(part.samples);
  }
  /* Corrupt bytes never take it down; the outcome is audio or a format
   * error. */
  uint8_t *copy = malloc(mp3->len);
  assert(copy);
  for (unsigned trial = 0; trial < 300u; ++trial) {
    memcpy(copy, mp3->data, mp3->len);
    for (unsigned k = 0; k < 1u + trial % 40u; ++k)
      copy[tag + next_rand() % (mp3->len - tag)] = (uint8_t)next_rand();
    decoded_t part = decode(copy, mp3->len);
    assert(part.rc == H2_PAL_EXIT || part.rc == H2_PAL_ERR_FORMAT);
    free(part.samples);
  }
  free(copy);
  free(full.samples);
}

static void test_mp3_vbr(const blob_t *mp3) {
  /* 3 s at 22.05 kHz, Xing tag with a TOC: 48000 samples. */
  decoded_t full = decode(mp3->data, mp3->len);
  assert(full.rc == H2_PAL_EXIT && full.count == 48000);
  assert(tone_share(full.samples + 2000, 44000, 1000.0) > 0.98);
  /* A VBR estimate aims 5 s early, which here is the first frame: the
   * landing is then exact. */
  for (int resync = 0; resync < 2; ++resync) {
    decoded_t part =
        decode_ex(mp3->data, mp3->len, 4096u, 1500, resync != 0, 3000);
    assert(part.rc == H2_PAL_EXIT && part.origin == 24000 &&
           part.count == 24000);
    free(part.samples);
  }
  free(full.samples);
}

static void test_mp3_untagged(const blob_t *mp3) {
  /* MPEG-2.5 at 8 kHz with no tag: every frame is audio, decoder delay
   * included. */
  assert(mp3->len % 72u == 0);
  const size_t frames = mp3->len / 72u;
  decoded_t full = decode(mp3->data, mp3->len);
  assert(full.rc == H2_PAL_EXIT && full.count == frames * 576u * 2u);
  assert(tone_share(full.samples + 3000, 12000, 1000.0) > 0.95);
  for (int resync = 0; resync < 2; ++resync) {
    decoded_t part =
        decode_ex(mp3->data, mp3->len, 4096u, 500, resync != 0, 1000);
    assert(part.rc == H2_PAL_EXIT && part.origin == 8000 &&
           part.count == full.count - 8000u);
    free(part.samples);
  }
  /* Junk between frames loses sync for a moment, not the track; junk after
   * a tag but before the first frame is skipped. */
  uint8_t *junk = malloc(mp3->len + 1000u);
  assert(junk);
  memcpy(junk, mp3->data, 72u * 5u);
  memset(junk + 72u * 5u, 0x55, 500);
  memcpy(junk + 72u * 5u + 500u, mp3->data + 72u * 5u, mp3->len - 72u * 5u);
  decoded_t lost = decode(junk, mp3->len + 500u);
  assert(lost.rc == H2_PAL_EXIT && lost.count == full.count);
  free(lost.samples);
  size_t n = make_id3(junk, 20, false);
  memset(junk + n, 0, 300);
  memcpy(junk + n + 300u, mp3->data, mp3->len);
  decoded_t late = decode(junk, n + 300u + mp3->len);
  assert_same(&late, &full);
  free(late.samples);
  /* A trailing tag of any size (APEv2 with cover art) after the last frame
   * ends the track normally. */
  uint8_t *tail = malloc(mp3->len + 100000u);
  assert(tail);
  memcpy(tail, mp3->data, mp3->len);
  memcpy(tail + mp3->len, "APETAGEX", 8);
  memset(tail + mp3->len + 8u, 0x41, 100000u - 8u);
  decoded_t tagged = decode(tail, mp3->len + 100000u);
  assert_same(&tagged, &full);
  free(tagged.samples);
  free(tail);
  /* Zeros in front of the first frame, with no tag, are not an MP3. */
  memset(junk, 0, 300);
  memcpy(junk + 300, mp3->data, mp3->len);
  assert(decode_rc(junk, 300u + mp3->len) == H2_PAL_ERR_UNSUPPORTED);
  free(junk);
  free(full.samples);
}

/* --- Format detection and the Ogg/Opus path ------------------------------ */

static void test_sniff(void) {
  assert(decode_rc((const uint8_t *)"", 0) == H2_PAL_ERR_FORMAT);
  const char html[] = "<!DOCTYPE html><html></html>";
  assert(decode_rc((const uint8_t *)html, sizeof(html) - 1u) ==
         H2_PAL_ERR_UNSUPPORTED);
  /* Layer II and free-format Layer III headers are not accepted as MP3. */
  const uint8_t layer2[8] = {0xFF, 0xFD, 0x90, 0x00};
  assert(decode_rc(layer2, sizeof(layer2)) == H2_PAL_ERR_UNSUPPORTED);
  const uint8_t free_format[8] = {0xFF, 0xFB, 0x00, 0x00};
  assert(decode_rc(free_format, sizeof(free_format)) ==
         H2_PAL_ERR_UNSUPPORTED);
  /* An ID3 tag that runs past the end, or with nothing after it. */
  uint8_t tag[400];
  make_id3(tag, 390, false);
  assert(decode_rc(tag, 200) == H2_PAL_ERR_FORMAT);
  assert(decode_rc(tag, 400) == H2_PAL_ERR_FORMAT);
  /* Ogg/Opus through the front end is the Ogg/Opus decoder. */
  static fixture_t f;
  memset(&f, 0, sizeof(f));
  make_packet(&f, 1);
  headers(&f, 7, 1, 312, 0);
  paginate(&f, 7, 50, 0, 0);
  decoded_t out = decode(f.bytes, f.len);
  assert(out.rc == H2_PAL_EXIT && out.count == 50u * 320u - 104u);
  free(out.samples);
}

int main(int argc, char **argv) {
  assert(argc == 4);
  test_resample();
  test_wav_encodings();
  test_wav_resample_and_seek();
  blob_t cbr = load(argv[1]), vbr = load(argv[2]), untagged = load(argv[3]);
  test_mp3_cbr(&cbr);
  test_mp3_vbr(&vbr);
  test_mp3_untagged(&untagged);
  test_sniff();
  free(cbr.data);
  free(vbr.data);
  free(untagged.data);
  puts("gizclaw audio decoder tests passed");
  return 0;
}
