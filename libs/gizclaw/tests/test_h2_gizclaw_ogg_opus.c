#include "h2_gizclaw_ogg_opus_internal.h"
#include "opus.h"

// These tests use assertions for both checks and the operations under test.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct memory {
  size_t live, calls, fail_at;
} memory_t;

static void *allocate(void *ctx, size_t len) {
  memory_t *m = ctx;
  if (++m->calls == m->fail_at)
    return NULL;
  void *p = malloc(len);
  if (p != NULL)
    ++m->live;
  return p;
}

static void release(void *ctx, void *p) {
  memory_t *m = ctx;
  if (p != NULL) {
    assert(m->live > 0);
    --m->live;
    free(p);
  }
}

static const h2_pal_mem_vtable_t memory_vtable = {.alloc = allocate,
                                                  .free = release};

#include "ogg_opus_fixture.h"

typedef struct stream_input {
  const uint8_t *data;
  size_t length, offset;
} stream_input_t;
static h2_pal_result_t stream_read(void *user, uint8_t *out, size_t capacity, size_t *length) {
  stream_input_t *input = user;
  *length = input->length - input->offset;
  if (*length > capacity) *length = capacity;
  if (*length > 7) *length = 7; /* Split headers, laces and packet bodies. */
  memcpy(out, input->data + input->offset, *length);
  input->offset += *length;
  return *length ? H2_PAL_OK : H2_PAL_EXIT;
}
static size_t decode_mode(fixture_t *f, h2_pal_result_t expected, size_t fail_at,
                           bool streaming) {
  memory_t mem = {.fail_at = fail_at};
  h2_pal_mem_api_t allocator = {.user = &mem, .vtable = &memory_vtable};
  h2_gizclaw_ogg_opus_t *decoder = NULL;
  stream_input_t input = {.data = f->bytes, .length = f->len};
  h2_pal_result_t rc = streaming ?
      h2_gizclaw_ogg_opus_create_reader(&allocator, stream_read, &input, &decoder) :
      h2_gizclaw_ogg_opus_create(&allocator, f->bytes, f->len, &decoder);
  uint8_t pcm[H2_GIZCLAW_OGG_OPUS_PCM_BYTES];
  size_t total = 0, nonzero = 0, steps = 0;
  while (rc == H2_PAL_OK) {
    size_t len;
    rc = h2_gizclaw_ogg_opus_next(decoder, pcm, sizeof(pcm), &len);
    assert(++steps < 1000);
    total += len;
    for (size_t i = 0; i < len; ++i)
      nonzero += pcm[i] != 0;
  }
  if (rc != expected)
    fprintf(stderr, "decode rc=%d expected=%d len=%zu output=%zu\n", rc,
            expected, f->len, total);
  assert(rc == expected);
  if (total > 0)
    assert(nonzero > 0);
  if (decoder) {
    size_t len = SIZE_MAX;
    assert(h2_gizclaw_ogg_opus_next(decoder, pcm, sizeof(pcm), &len) == rc);
    assert(len == 0);
  }
  h2_gizclaw_ogg_opus_destroy(decoder);
  assert(mem.live == 0);
  return total;
}

static size_t decode(fixture_t *f, h2_pal_result_t expected, size_t fail_at) {
  size_t bytes = decode_mode(f, expected, fail_at, false);
  if (!fail_at && f->len)
    assert(decode_mode(f, expected, 0, true) == bytes);
  return bytes;
}

static void test_valid(void) {
  fixture_t f = {0};
  make_packet(&f, 1);
  headers(&f, 123, 1, 312, 0);
  packet_page(&f, 0, 960, 123, 2, f.packet, f.packet_len);
  packet_page(&f, 4, 1800, 123, 3, f.packet, f.packet_len);
  assert(decode(&f, H2_PAL_EXIT, 0) == (1800 - 312) / 3 * 2);
  for (size_t fail = 1; fail <= 3; ++fail)
    decode(&f, H2_PAL_ERR_NO_MEMORY, fail);
  /* Chained stereo stream, downmixed to mono; skip/end use sample positions,
   * not channel count. A fractional pre-skip rounds up on the output grid. */
  make_packet(&f, 2);
  headers(&f, 124, 2, 313, -256);
  packet_page(&f, 4, 901, 124, 2, f.packet, f.packet_len);
  assert(decode(&f, H2_PAL_EXIT, 0) == 992 + (300 - 105) * 2);
  /* A cropped first-and-last audio page may start at a nonzero timestamp. */
  f.len = 0;
  headers(&f, 200, 1, 0, 0);
  packet_page(&f, 4, 5000, 200, 2, f.packet, f.packet_len);
  assert(decode(&f, H2_PAL_EXIT, 0) == 640);
}

static void test_continued(void) {
  fixture_t f = {0};
  make_packet(&f, 1);
  headers(&f, 123, 1, 0, 0);
  uint8_t first[] = {255}, last[] = {(uint8_t)(f.packet_len - 255)};
  assert(f.packet_len < 510);
  page(&f, 0, UINT64_MAX, 123, 2, first, 1, f.packet, 255);
  page(&f, 5, 960, 123, 3, last, 1, f.packet + 255, f.packet_len - 255);
  assert(decode(&f, H2_PAL_EXIT, 0) == 640);
}

static void test_multi_packet_trim(void) {
  fixture_t f = {0};
  make_packet(&f, 1);
  headers(&f, 123, 1, 0, 0);
  uint8_t packets[3000], laces[4] = {255, (uint8_t)(f.packet_len - 255), 255,
                                     (uint8_t)(f.packet_len - 255)};
  memcpy(packets, f.packet, f.packet_len);
  memcpy(packets + f.packet_len, f.packet, f.packet_len);
  page(&f, 4, 900, 123, 2, laces, 4, packets, f.packet_len * 2);
  assert(decode(&f, H2_PAL_EXIT, 0) == 600);
}

static void test_continued_tags_and_page_timestamps(void) {
  fixture_t f = {0};
  make_packet(&f, 1);
  headers(&f, 123, 1, 0, 0);
  f.len = 47; /* Keep only the OpusHead page. */
  uint8_t tags[256] = "OpusTags";
  put32(tags + 8, 240);
  memset(tags + 12, 'v', 240);
  const uint8_t first[] = {255}, last[] = {1};
  page(&f, 0, UINT64_MAX, 123, 1, first, 1, tags, 255);
  page(&f, 1, 0, 123, 2, last, 1, tags + 255, 1);
  packet_page(&f, 0, 5000, 123, 3, f.packet, f.packet_len);
  packet_page(&f, 0, 5960, 123, 4, f.packet, f.packet_len);
  size_t final = packet_page(&f, 4, 6800, 123, 5, f.packet, f.packet_len);
  assert(decode(&f, H2_PAL_EXIT, 0) == 640 + 640 + 560);
  /* A later EOS may trim, but cannot extend beyond the decoded last page. */
  put64(f.bytes + final + 6, 7000);
  memset(f.bytes + final + 22, 0, 4);
  put32(f.bytes + final + 22, checksum(f.bytes + final, f.len - final));
  decode(&f, H2_PAL_ERR_FORMAT, 0);
}

static void test_all_truncations(void) {
  fixture_t f = {0};
  make_packet(&f, 1);
  headers(&f, 123, 1, 0, 0);
  packet_page(&f, 4, 960, 123, 2, f.packet, f.packet_len);
  size_t full = f.len;
  for (size_t len = 1; len < full; ++len) {
    f.len = len;
    decode(&f, H2_PAL_ERR_FORMAT, 0);
  }
}

static void test_invalid(void) {
  for (unsigned mode = 0; mode < 17; ++mode) {
    fixture_t f = {0};
    make_packet(&f, 1);
    headers(&f, 123, 1, mode == 10 ? 1000 : 0, 0);
    size_t audio = packet_page(&f, 4, 960, 123, 2, f.packet, f.packet_len);
    switch (mode) {
    case 0:
      --f.len;
      break; /* Truncated body. */
    case 1:
      f.bytes[f.len - 1] ^= 1;
      break; /* CRC. */
    case 2:
      f.bytes[audio + 5] = 0;
      break; /* No EOS. */
    case 3:
      put32(f.bytes + audio + 18, 4);
      break; /* Sequence gap. */
    case 4:
      put32(f.bytes + audio + 14, 124);
      break; /* Multiplexed stream. */
    case 5:
      f.bytes[audio + 5] = 5;
      break; /* Spurious continuation. */
    case 6:
      /* A non-final first audio page may not timestamp before its samples. */
      f.bytes[audio + 5] = 0;
      put64(f.bytes + audio + 6, 959);
      break;
    case 7:
      put64(f.bytes + audio + 6, UINT64_MAX);
      break;
    case 8:
      f.bytes[audio + 5] = 6;
      break; /* Repeated BOS. */
    case 9:
      f.len = audio;
      break; /* Header-only file. */
    case 10:
      break; /* EOS smaller than pre-skip. */
    case 11:
      f.bytes[audio + 4] = 1;
      break;
    case 12:
      f.bytes[audio + 5] = 12;
      break;
    case 13:
      f.bytes[audio + 27] = 0;
      break; /* Zero-byte Opus packet. */
    case 14:
      put64(f.bytes + audio + 6, (uint64_t)INT64_MAX + 1);
      break;
    case 15:
      f.bytes[0] = 'X';
      break;
    case 16: /* Header-only EOS followed by valid audio is not a stream. */
      f.bytes[5] = 6;
      memset(f.bytes + 22, 0, 4);
      put32(f.bytes + 22, checksum(f.bytes, 47));
      break;
    }
    if (mode != 0 && mode != 1 && mode != 9) {
      memset(f.bytes + audio + 22, 0, 4);
      put32(f.bytes + audio + 22, checksum(f.bytes + audio, f.len - audio));
    }
    decode(&f, H2_PAL_ERR_FORMAT, 0);
  }
}

/* The header pages come from the file; after seek() the reader switches to
 * `range`, the bytes a ranged GET would return. */
typedef struct seek_input {
  const uint8_t *file, *range;
  size_t file_len, range_len, offset;
  bool switched;
} seek_input_t;
static h2_pal_result_t seek_read(void *user, uint8_t *out, size_t capacity,
                                 size_t *length) {
  seek_input_t *input = user;
  const uint8_t *data = input->switched ? input->range : input->file;
  const size_t len = input->switched ? input->range_len : input->file_len;
  *length = len - input->offset;
  if (*length > capacity) *length = capacity;
  if (*length > 7) *length = 7;
  memcpy(out, data + input->offset, *length);
  input->offset += *length;
  return *length ? H2_PAL_OK : H2_PAL_EXIT;
}
typedef struct seek_result {
  h2_pal_result_t rc;
  uint64_t origin, emitted;
} seek_result_t;
/* Decode headers from the file, then seek. With a range the decoder resyncs
 * on it; without one it keeps reading the file sequentially. */
static seek_result_t seek_decode(const fixture_t *f, size_t header_len,
                                 const uint8_t *range, size_t range_len,
                                 uint64_t target_ms) {
  memory_t mem = {0};
  h2_pal_mem_api_t allocator = {.user = &mem, .vtable = &memory_vtable};
  seek_input_t input = {.file = f->bytes, .file_len = f->len,
                        .range = range, .range_len = range_len};
  h2_gizclaw_ogg_opus_t *decoder = NULL;
  assert(h2_gizclaw_ogg_opus_create_reader(&allocator, seek_read, &input,
                                           &decoder) == H2_PAL_OK);
  uint8_t pcm[H2_GIZCLAW_OGG_OPUS_PCM_BYTES];
  size_t len = 0;
  uint64_t origin = 0;
  assert(h2_gizclaw_ogg_opus_seek(decoder, 0, false) == H2_PAL_ERR_INVALID_STATE);
  while (!h2_gizclaw_ogg_opus_headers_done(decoder)) {
    assert(h2_gizclaw_ogg_opus_next(decoder, pcm, sizeof(pcm), &len) == H2_PAL_OK);
    assert(len == 0);
  }
  /* The reader consumed exactly the header pages. */
  assert(input.offset == header_len);
  if (range) {
    input.switched = true;
    input.offset = 0;
  }
  assert(h2_gizclaw_ogg_opus_seek(decoder, target_ms, range != NULL) == H2_PAL_OK);
  assert(h2_gizclaw_ogg_opus_seek(decoder, target_ms, false) ==
         H2_PAL_ERR_INVALID_STATE);
  seek_result_t result = {.rc = H2_PAL_OK};
  bool known = false;
  for (unsigned steps = 0; result.rc == H2_PAL_OK; ++steps) {
    assert(steps < 10000);
    result.rc = h2_gizclaw_ogg_opus_next(decoder, pcm, sizeof(pcm), &len);
    if (len) {
      /* The origin is known before the first sample leaves the decoder. */
      assert(h2_gizclaw_ogg_opus_origin(decoder, &origin));
      if (!known) result.origin = origin;
      assert(origin == result.origin);
      known = true;
    }
    result.emitted += len / 2;
  }
  if (result.rc == H2_PAL_EXIT && !known) {
    assert(h2_gizclaw_ogg_opus_origin(decoder, &origin));
    result.origin = origin;
  }
  h2_gizclaw_ogg_opus_destroy(decoder);
  assert(mem.live == 0);
  return result;
}
/* 50 × 20 ms, pre-skip 312, 100 samples of end trim: 16 kHz output is
 * (48000 - 100 - 312) / 3 rounded as the decoder rounds. */
static size_t seek_fixture(fixture_t *f, size_t page_body) {
  memset(f, 0, sizeof(*f));
  make_packet(f, 1);
  headers(f, 77, 1, 312, 0);
  return paginate(f, 77, 50, page_body, 100);
}

static void test_seek_sequential(void) {
  fixture_t f;
  const size_t header_len = seek_fixture(&f, 0);
  const uint64_t plain = decode(&f, H2_PAL_EXIT, 0) / 2;
  /* A 200 body: skip, then decode and discard, to exactly the target. */
  const uint64_t targets[] = {0, 1, 10, 79, 81, 100, 333, 500, 979, 980, 990};
  for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
    seek_result_t r = seek_decode(&f, header_len, NULL, 0, targets[i]);
    assert(r.rc == H2_PAL_EXIT);
    assert(r.origin == targets[i] * 16u);
    assert(r.origin + r.emitted == plain);
  }
  /* Past the validated end: nothing plays and the origin is the end. */
  seek_result_t r = seek_decode(&f, header_len, NULL, 0, 5000);
  assert(r.rc == H2_PAL_EXIT && r.emitted == 0 && r.origin == plain);
  /* Straddling packets are counted the same way. */
  const size_t split_header = seek_fixture(&f, 600);
  assert(split_header == header_len);
  assert(decode(&f, H2_PAL_EXIT, 0) / 2 == plain);
  r = seek_decode(&f, header_len, NULL, 0, 500);
  assert(r.rc == H2_PAL_EXIT && r.origin == 8000 && r.origin + r.emitted == plain);
  /* Only a reader decoder can resync; memory decoders seek sequentially. */
  memory_t mem = {0};
  h2_pal_mem_api_t allocator = {.user = &mem, .vtable = &memory_vtable};
  h2_gizclaw_ogg_opus_t *decoder = NULL;
  assert(h2_gizclaw_ogg_opus_create(&allocator, f.bytes, f.len, &decoder) == H2_PAL_OK);
  uint8_t pcm[H2_GIZCLAW_OGG_OPUS_PCM_BYTES];
  size_t len;
  while (!h2_gizclaw_ogg_opus_headers_done(decoder))
    assert(h2_gizclaw_ogg_opus_next(decoder, pcm, sizeof(pcm), &len) == H2_PAL_OK);
  assert(h2_gizclaw_ogg_opus_seek(decoder, 0, true) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_gizclaw_ogg_opus_seek(NULL, 0, false) == H2_PAL_ERR_INVALID_ARG);
  assert(!h2_gizclaw_ogg_opus_origin(NULL, &r.origin));
  assert(h2_gizclaw_ogg_opus_seek(decoder, 200, false) == H2_PAL_OK);
  assert(!h2_gizclaw_ogg_opus_origin(decoder, &r.origin));
  h2_gizclaw_ogg_opus_destroy(decoder);
  assert(mem.live == 0);
}

/* Page start offsets and the granule at each page's first packet start. */
static size_t page_offsets(const fixture_t *f, size_t from, size_t *out,
                           size_t capacity) {
  size_t count = 0;
  for (size_t at = from; at < f->len && count < capacity; ++count) {
    out[count] = at;
    size_t body = 0;
    for (size_t i = 0; i < f->bytes[at + 26]; ++i)
      body += f->bytes[at + 27 + i];
    at += 27u + f->bytes[at + 26] + body;
  }
  return count;
}

static void test_seek_resync_every_offset(void) {
  for (unsigned variant = 0; variant < 2; ++variant) {
    fixture_t f;
    const size_t header_len = seek_fixture(&f, variant ? 600 : 0);
    const uint64_t plain = decode(&f, H2_PAL_EXIT, 0) / 2;
    size_t pages[128];
    const size_t count = page_offsets(&f, header_len, pages, 128);
    /* Byte offsets a ranged request could start at: every byte across three
     * pages, a stride elsewhere, and every byte of the tail. The origin must
     * be exact (it and the rest of the output add up to a plain playback);
     * only a start past the last page that can anchor the timeline fails,
     * with FORMAT. */
    size_t landed = 0, failed = 0;
    for (size_t from = header_len; from < f.len;
         from += (from >= pages[20] && from < pages[23]) ||
                         from >= pages[count - 3] ? 1 : 7) {
      seek_result_t r = seek_decode(&f, header_len, f.bytes + from, f.len - from, 0);
      if (r.rc == H2_PAL_ERR_FORMAT) {
        /* Variant 0: page count - 2 is the last non-EOS page. */
        assert(from > pages[count - (variant ? 3 : 2)]);
        ++failed;
        continue;
      }
      if (!variant)
        assert(from <= pages[count - 2]);
      assert(r.rc == H2_PAL_EXIT);
      assert(r.origin + r.emitted == plain);
      size_t page = 0;
      while (page < count && pages[page] < from) ++page;
      assert(page + 1 < count);
      if (!variant) {
        /* One packet per page: page k starts at 960 × k, pre-skip 312, and
         * any start after 0 pre-rolls 80 ms. */
        const uint64_t start = 960u * page / 3u;
        const uint64_t expected =
            (start > 104u ? start - 104u : 0u) + (page ? 1280u : 0u);
        /* A pre-roll past the trimmed end leaves the origin at the end. */
        assert(r.origin == (expected < plain ? expected : plain));
      }
      ++landed;
    }
    assert(landed > 0 && failed > 0);
  }
  /* Landing on a known page gives its granule-derived start exactly: page
   * k (one packet per page) starts at 960 × k, pre-skip 312. */
  fixture_t f;
  const size_t header_len = seek_fixture(&f, 0);
  size_t pages[64];
  page_offsets(&f, header_len, pages, 64);
  seek_result_t r = seek_decode(&f, header_len, f.bytes + pages[20],
                                f.len - pages[20], 0);
  assert(r.rc == H2_PAL_EXIT);
  assert(r.origin == (960u * 20u) / 3u - (312u + 2u) / 3u + 1280u);
  /* A later target is decoded and discarded up to exactly the target. */
  r = seek_decode(&f, header_len, f.bytes + pages[20], f.len - pages[20], 700);
  assert(r.rc == H2_PAL_EXIT && r.origin == 700u * 16u);
}

static void test_seek_resync_rejects_fake_pages(void) {
  fixture_t f;
  const size_t header_len = seek_fixture(&f, 0);
  const uint64_t plain = decode(&f, H2_PAL_EXIT, 0) / 2;
  size_t pages[64];
  page_offsets(&f, header_len, pages, 64);
  const uint64_t expected = (960u * 10u) / 3u - (312u + 2u) / 3u + 1280u;
  static uint8_t range[65536];
  for (unsigned mode = 0; mode < 4; ++mode) {
    /* Garbage, then a candidate page, then the real stream from page 10. */
    size_t len = 0;
    memcpy(range, "xOgOggOgg", 9);
    len = 9;
    uint8_t *fake = range + len;
    memcpy(fake, f.bytes + pages[3], pages[4] - pages[3]);
    size_t fake_len = pages[4] - pages[3];
    if (mode == 0) {
      /* Bad CRC, and lacing that claims a body reaching into the real
       * pages: the rejected candidate's bytes must be rescanned in place. */
      fake[22] ^= 0x5a;
      fake[26] = 12;
      memset(fake + 27, 255, 12);
      fake_len = 27 + 12;
    } else if (mode == 1) {
      put32(fake + 14, 99); /* Another logical stream, valid CRC. */
      memset(fake + 22, 0, 4);
      put32(fake + 22, checksum(fake, fake_len));
    } else if (mode == 2) {
      fake[5] = 2; /* A BOS page cannot be mid-stream. */
      memset(fake + 22, 0, 4);
      put32(fake + 22, checksum(fake, fake_len));
    } else {
      fake[4] = 1; /* Unknown version. */
    }
    len += fake_len;
    memcpy(range + len, f.bytes + pages[10], f.len - pages[10]);
    len += f.len - pages[10];
    seek_result_t r = seek_decode(&f, header_len, range, len, 0);
    assert(r.rc == H2_PAL_EXIT);
    assert(r.origin == expected && r.origin + r.emitted == plain);
  }
  /* Nothing but garbage: bounded scan, FORMAT, no landing. */
  memset(range, 'g', sizeof(range));
  seek_result_t r = seek_decode(&f, header_len, range, sizeof(range), 0);
  assert(r.rc == H2_PAL_ERR_FORMAT);
  /* Only the EOS page is left: it cannot anchor the timeline. */
  size_t count = page_offsets(&f, header_len, pages, 64);
  r = seek_decode(&f, header_len, f.bytes + pages[count - 1],
                  f.len - pages[count - 1], 0);
  assert(r.rc == H2_PAL_ERR_FORMAT);
}

/* Host-only interoperability oracle: decode an external fixture to raw PCM. */
static int decode_file(const char *source, const char *target) {
  FILE *in = fopen(source, "rb");
  assert(in && fseek(in, 0, SEEK_END) == 0);
  long length = ftell(in);
  assert(length > 0 && fseek(in, 0, SEEK_SET) == 0);
  uint8_t *data = malloc((size_t)length);
  assert(data && fread(data, 1, (size_t)length, in) == (size_t)length);
  fclose(in);
  FILE *out = fopen(target, "wb");
  assert(out);
  memory_t mem = {0};
  h2_pal_mem_api_t allocator = {.user = &mem, .vtable = &memory_vtable};
  h2_gizclaw_ogg_opus_t *d;
  assert(h2_gizclaw_ogg_opus_create(&allocator, data, (size_t)length, &d) ==
         H2_PAL_OK);
  uint8_t pcm[H2_GIZCLAW_OGG_OPUS_PCM_BYTES];
  h2_pal_result_t rc;
  size_t total = 0;
  do {
    size_t len;
    rc = h2_gizclaw_ogg_opus_next(d, pcm, sizeof(pcm), &len);
    assert(fwrite(pcm, 1, len, out) == len);
    total += len;
  } while (rc == H2_PAL_OK);
  fclose(out);
  h2_gizclaw_ogg_opus_destroy(d);
  free(data);
  assert(mem.live == 0);
  printf("decoded_bytes=%zu result=%d\n", total, rc);
  return rc == H2_PAL_EXIT ? 0 : 1;
}

/* Cover art makes OpusTags far larger than the 64 KiB packet ceiling. The
 * reader keeps the first 64 KiB, checks the magic and drops the rest; the
 * memory decoder keeps and validates all of it; both play the same audio,
 * and a timed start still lands exactly. */
static void test_oversized_tags(void) {
  static fixture_t f;
  static uint8_t tags[70000];
  for (unsigned mode = 0; mode < 2; ++mode) {
    memset(&f, 0, sizeof(f));
    make_packet(&f, 1);
    headers(&f, 123, 1, 312, 0);
    f.len = 47; /* Keep only the OpusHead page. */
    memset(tags, 'p', sizeof(tags));
    memcpy(tags, mode ? "OpusTagZ" : "OpusTags", 8);
    put32(tags + 8, (uint32_t)(sizeof(tags) - 16u));
    put32(tags + sizeof(tags) - 4u, 0);
    uint8_t laces[255];
    memset(laces, 255, sizeof(laces));
    page(&f, 0, UINT64_MAX, 123, 1, laces, 255, tags, 255u * 255u);
    const size_t rest = sizeof(tags) - 255u * 255u;
    const size_t count = rest / 255u + 1u;
    laces[count - 1u] = (uint8_t)(rest % 255u);
    page(&f, 1, 0, 123, 2, laces, count, tags + 255u * 255u, rest);
    const size_t header_len = f.len;
    for (uint32_t i = 0; i < 20; ++i)
      packet_page(&f, i + 1 == 20 ? 4 : 0, 960u * (i + 1u), 123, 3 + i,
                  f.packet, f.packet_len);
    if (mode) {
      decode(&f, H2_PAL_ERR_FORMAT, 0); /* No magic, oversized or not. */
      continue;
    }
    const uint64_t plain = decode(&f, H2_PAL_EXIT, 0) / 2u;
    assert(plain == (19200u - 312u) / 3u);
    seek_result_t r = seek_decode(&f, header_len, NULL, 0, 200);
    assert(r.rc == H2_PAL_EXIT && r.origin == 3200u && r.origin + r.emitted == plain);
  }
}

/* Host-only: exercise seek on an external Ogg/Opus file
 * (`h2_gizclaw_ogg_opus_test --seek-sweep file.ogg`). Every resync offset and
 * sequential target must give an origin that, with the samples after it, adds
 * up to the whole-file decode, and output that lines up best at shift 0 with
 * that decode at the origin. */
typedef struct sweep_file {
  uint8_t *data;
  size_t len, header_len;
  int16_t *plain;
  size_t plain_samples;
} sweep_file_t;
static seek_result_t sweep_one(const sweep_file_t *file, bool resync,
                               size_t from, uint64_t target_ms, int16_t *out,
                               size_t out_cap) {
  memory_t mem = {0};
  h2_pal_mem_api_t allocator = {.user = &mem, .vtable = &memory_vtable};
  seek_input_t input = {.file = file->data, .file_len = file->len,
                        .range = file->data + from, .range_len = file->len - from};
  h2_gizclaw_ogg_opus_t *decoder = NULL;
  assert(h2_gizclaw_ogg_opus_create_reader(&allocator, seek_read, &input,
                                           &decoder) == H2_PAL_OK);
  uint8_t pcm[H2_GIZCLAW_OGG_OPUS_PCM_BYTES];
  size_t len = 0;
  seek_result_t result = {.rc = H2_PAL_OK};
  while (result.rc == H2_PAL_OK && !h2_gizclaw_ogg_opus_headers_done(decoder))
    result.rc = h2_gizclaw_ogg_opus_next(decoder, pcm, sizeof(pcm), &len);
  if (result.rc == H2_PAL_OK) {
    if (resync) {
      input.switched = true;
      input.offset = 0;
    }
    assert(h2_gizclaw_ogg_opus_seek(decoder, target_ms, resync) == H2_PAL_OK);
  }
  bool known = false;
  while (result.rc == H2_PAL_OK) {
    result.rc = h2_gizclaw_ogg_opus_next(decoder, pcm, sizeof(pcm), &len);
    if (len && !known) {
      assert(h2_gizclaw_ogg_opus_origin(decoder, &result.origin));
      known = true;
    }
    if (result.emitted * 2u + len <= out_cap * 2u)
      memcpy(out + result.emitted, pcm, len);
    result.emitted += len / 2u;
  }
  if (result.rc == H2_PAL_EXIT && !known)
    assert(h2_gizclaw_ogg_opus_origin(decoder, &result.origin));
  h2_gizclaw_ogg_opus_destroy(decoder);
  assert(mem.live == 0);
  return result;
}
/* Signal-to-noise power ratio (not dB, so no libm) of `got` against the
 * plain decode at `origin + shift` over n samples. */
static double sweep_snr(const sweep_file_t *file, const int16_t *got, size_t n,
                        uint64_t origin, int shift) {
  double signal = 0, noise = 0;
  for (size_t i = 0; i < n; ++i) {
    const double a = file->plain[(int64_t)origin + shift + (int64_t)i], b = got[i];
    signal += a * a;
    noise += (a - b) * (a - b);
  }
  if (noise == 0)
    return 1e20;
  return signal / noise;
}
static int seek_sweep(const char *path) {
  sweep_file_t file = {0};
  FILE *in = fopen(path, "rb");
  assert(in && fseek(in, 0, SEEK_END) == 0);
  file.len = (size_t)ftell(in);
  assert(file.len > 0 && fseek(in, 0, SEEK_SET) == 0);
  file.data = malloc(file.len);
  assert(file.data && fread(file.data, 1, file.len, in) == file.len);
  fclose(in);
  /* Plain decode: the oracle, and where the header pages end. */
  memory_t mem = {0};
  h2_pal_mem_api_t allocator = {.user = &mem, .vtable = &memory_vtable};
  seek_input_t input = {.file = file.data, .file_len = file.len};
  h2_gizclaw_ogg_opus_t *decoder = NULL;
  assert(h2_gizclaw_ogg_opus_create_reader(&allocator, seek_read, &input,
                                           &decoder) == H2_PAL_OK);
  uint8_t pcm[H2_GIZCLAW_OGG_OPUS_PCM_BYTES];
  size_t len = 0, cap = file.len * 64u + 16000u;
  file.plain = malloc(cap * 2u);
  assert(file.plain);
  h2_pal_result_t rc = H2_PAL_OK;
  while (rc == H2_PAL_OK) {
    rc = h2_gizclaw_ogg_opus_next(decoder, pcm, sizeof(pcm), &len);
    if (!file.header_len && h2_gizclaw_ogg_opus_headers_done(decoder))
      file.header_len = input.offset;
    assert(file.plain_samples + len / 2u <= cap);
    memcpy(file.plain + file.plain_samples, pcm, len);
    file.plain_samples += len / 2u;
  }
  h2_gizclaw_ogg_opus_destroy(decoder);
  printf("file=%s bytes=%zu header=%zu plain_rc=%d duration_ms=%zu\n", path,
         file.len, file.header_len, rc, file.plain_samples / 16u);
  if (rc != H2_PAL_EXIT) {
    printf("SWEEP UNSUPPORTED plain decode rc=%d\n", rc);
    return 2;
  }
  int16_t *out = malloc(file.plain_samples * 2u + 4096u);
  assert(out);
  unsigned landed = 0, format = 0, bad = 0;
  double worst = 1e30;
  /* 97 resync offsets across the audio, then 41 sequential targets. */
  for (unsigned k = 0; k < 97u + 41u; ++k) {
    const bool resync = k < 97u;
    const size_t from = resync ? file.header_len +
        (size_t)((double)(file.len - file.header_len) * k / 97.0) : 0;
    const uint64_t target = resync ? 0 :
        (uint64_t)((double)file.plain_samples / 16.0 * (k - 97u) / 41.0);
    seek_result_t r = sweep_one(&file, resync, from, target, out,
                                file.plain_samples + 2048u);
    if (r.rc == H2_PAL_ERR_FORMAT && resync) {
      ++format; /* Past the last page that can anchor. */
      continue;
    }
    bool ok = r.rc == H2_PAL_EXIT && r.origin + r.emitted == file.plain_samples;
    if (!resync)
      ok = ok && (target * 16u >= file.plain_samples
                      ? r.origin == file.plain_samples
                      : r.origin == target * 16u);
    const size_t n = r.emitted < 16000u ? r.emitted : 16000u;
    if (ok && n >= 1600u) {
      /* Exact alignment: shift 0 beats every other shift within 20 ms. */
      const double at = sweep_snr(&file, out, n, r.origin, 0);
      for (int shift = -320; shift <= 320 && ok; ++shift)
        if (shift && (int64_t)r.origin + shift >= 0 &&
            r.origin + (uint64_t)shift + n <= file.plain_samples &&
            sweep_snr(&file, out, n, r.origin, shift) >= at)
          ok = false;
      if (at < worst)
        worst = at;
    }
    if (resync && ok)
      ++landed;
    if (!ok) {
      ++bad;
      printf("BAD %s from=%zu target=%llu rc=%d origin=%llu emitted=%llu\n",
             resync ? "resync" : "sequential", from,
             (unsigned long long)target, r.rc, (unsigned long long)r.origin,
             (unsigned long long)r.emitted);
    }
  }
  printf("resync landed=%u format_at_tail=%u sequential=41 bad=%u "
         "worst_snr_ratio=%.1f\n", landed, format, bad, worst);
  printf("%s\n", bad ? "SWEEP FAIL" : "SWEEP PASS");
  free(out);
  free(file.plain);
  free(file.data);
  return bad ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--seek-sweep") == 0)
    return seek_sweep(argv[2]);
  if (argc == 3)
    return decode_file(argv[1], argv[2]);
  assert(argc == 1);
  test_valid();
  test_continued();
  test_multi_packet_trim();
  test_continued_tags_and_page_timestamps();
  test_all_truncations();
  test_invalid();
  test_seek_sequential();
  test_seek_resync_every_offset();
  test_seek_resync_rejects_fake_pages();
  test_oversized_tags();
  puts("Ogg/Opus decoder tests passed");
  return 0;
}
