#ifndef GIZCLAW_TEST_OGG_OPUS_FIXTURE_H
#define GIZCLAW_TEST_OGG_OPUS_FIXTURE_H

#include "opus.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct fixture {
  uint8_t bytes[32768];
  size_t len;
  uint8_t packet[1500];
  size_t packet_len;
} fixture_t;

static void put32(uint8_t *p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i)
    p[i] = (uint8_t)(n >> (8 * i));
}

static void put64(uint8_t *p, uint64_t n) {
  put32(p, (uint32_t)n);
  put32(p + 4, (uint32_t)(n >> 32));
}

/* Separate table-driven Ogg checksum oracle, not the decoder implementation. */
static uint32_t checksum(const uint8_t *p, size_t len) {
  uint32_t table[256];
  for (unsigned i = 0; i < 256; ++i) {
    uint32_t r = i << 24;
    for (unsigned j = 0; j < 8; ++j)
      r = r & 0x80000000u ? (r << 1) ^ 0x04c11db7u : r << 1;
    table[i] = r;
  }
  uint32_t r = 0;
  for (size_t i = 0; i < len; ++i)
    r = (r << 8) ^ table[(r >> 24) ^ p[i]];
  return r;
}

static size_t page(fixture_t *f, unsigned flags, uint64_t granule,
                   uint32_t serial, uint32_t sequence, const uint8_t *laces,
                   size_t count, const uint8_t *data, size_t len) {
  size_t start = f->len, size = 27 + count + len;
  assert(count <= 255 && size <= sizeof(f->bytes) - start);
  uint8_t *p = f->bytes + start;
  memset(p, 0, size);
  memcpy(p, "OggS", 4);
  p[5] = (uint8_t)flags;
  put64(p + 6, granule);
  put32(p + 14, serial);
  put32(p + 18, sequence);
  p[26] = (uint8_t)count;
  if (count)
    memcpy(p + 27, laces, count);
  if (len)
    memcpy(p + 27 + count, data, len);
  put32(p + 22, checksum(p, size));
  f->len += size;
  return start;
}

static size_t packet_page(fixture_t *f, unsigned flags, uint64_t granule,
                          uint32_t serial, uint32_t sequence,
                          const uint8_t *packet, size_t len) {
  uint8_t laces[255];
  size_t count = len / 255 + 1;
  assert(count <= sizeof(laces));
  memset(laces, 255, count);
  laces[count - 1] = (uint8_t)(len % 255);
  return page(f, flags, granule, serial, sequence, laces, count, packet, len);
}

static void headers(fixture_t *f, uint32_t serial, unsigned channels,
                    unsigned skip, int gain) {
  uint8_t head[19] = "OpusHead";
  head[8] = 1;
  head[9] = (uint8_t)channels;
  head[10] = (uint8_t)skip;
  head[11] = (uint8_t)(skip >> 8);
  put32(head + 12, 48000);
  head[16] = (uint8_t)gain;
  head[17] = (uint8_t)((unsigned)gain >> 8);
  packet_page(f, 2, 0, serial, 0, head, sizeof(head));
  const uint8_t tags[16] = "OpusTags";
  packet_page(f, 0, 0, serial, 1, tags, sizeof(tags));
}

static void make_packet(fixture_t *f, unsigned channels) {
  int rc;
  OpusEncoder *encoder =
      opus_encoder_create(16000, (int)channels, OPUS_APPLICATION_AUDIO, &rc);
  assert(encoder && rc == OPUS_OK);
  assert(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(128000)) == OPUS_OK);
  assert(opus_encoder_ctl(encoder, OPUS_SET_VBR(0)) == OPUS_OK);
  opus_int16 samples[640];
  for (unsigned i = 0; i < 320; ++i)
    for (unsigned c = 0; c < channels; ++c)
      samples[i * channels + c] = (opus_int16)((int)(i % 32) * 1000 - 16000);
  int len = opus_encode(encoder, samples, 320, f->packet, sizeof(f->packet));
  assert(len > 255 && (size_t)len < sizeof(f->packet));
  f->packet_len = (size_t)len;
  opus_encoder_destroy(encoder);
}

#endif
