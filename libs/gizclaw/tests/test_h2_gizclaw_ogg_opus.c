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

static size_t decode(fixture_t *f, h2_pal_result_t expected, size_t fail_at) {
  memory_t mem = {.fail_at = fail_at};
  h2_pal_mem_api_t allocator = {.user = &mem, .vtable = &memory_vtable};
  h2_gizclaw_ogg_opus_t *decoder = NULL;
  h2_pal_result_t rc =
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

int main(int argc, char **argv) {
  if (argc == 3)
    return decode_file(argv[1], argv[2]);
  assert(argc == 1);
  test_valid();
  test_continued();
  test_multi_packet_trim();
  test_continued_tags_and_page_timestamps();
  test_all_truncations();
  test_invalid();
  puts("Ogg/Opus decoder tests passed");
  return 0;
}
