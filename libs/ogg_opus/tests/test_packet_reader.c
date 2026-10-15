#include "h2_ogg_opus.h"
#include "opus.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
  uint8_t file[1024];
  size_t length, offset, quantum, largest;
  bool fail;
  size_t fail_offset;
} fixture_t;

static void *allocate(void *user, size_t bytes) {
  fixture_t *f = user;
  if (bytes > f->largest) f->largest = bytes;
  return malloc(bytes);
}
static void release(void *user, void *data) { (void)user; free(data); }
static const h2_pal_mem_vtable_t memory = {.alloc = allocate, .free = release};

static h2_pal_result_t read_bytes(void *user, uint8_t *out, size_t cap,
                                  size_t *out_len) {
  fixture_t *f = user;
  *out_len = 0;
  if (f->fail || (f->fail_offset && f->offset >= f->fail_offset)) {
    f->fail = false; f->fail_offset = 0; return H2_PAL_ERR_IO;
  }
  size_t n = f->length - f->offset;
  if (!n) return H2_PAL_EXIT;
  if (n > cap) n = cap;
  if (n > f->quantum) n = f->quantum;
  memcpy(out, f->file + f->offset, n);
  f->offset += n;
  *out_len = n;
  return H2_PAL_OK;
}
static void le32(uint8_t *p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8u*i));
}
static uint32_t crc(const uint8_t *p, size_t n) {
  uint32_t value = 0;
  for (size_t i = 0; i < n; ++i) {
    value ^= (uint32_t)((i >= 22 && i < 26) ? 0 : p[i]) << 24;
    for (unsigned bit = 0; bit < 8; ++bit)
      value = (value << 1) ^ ((value & 0x80000000u) ? 0x04c11db7u : 0);
  }
  return value;
}
static void page(fixture_t *f, unsigned seq, unsigned flags,
                  uint32_t granule, const uint8_t *packet, size_t bytes) {
  const size_t segments = bytes / 255u + 1u;
  const size_t header = 27u + segments;
  assert(bytes <= 512 && f->length + header + bytes <= sizeof(f->file));
  uint8_t *p = f->file + f->length;
  memset(p, 0, header);
  memcpy(p, "OggS", 4); p[5] = (uint8_t)flags;
  le32(p + 6, granule); le32(p + 14, 1); le32(p + 18, seq);
  p[26] = (uint8_t)segments;
  memset(p + 27, 255, segments - 1u);
  p[header - 1u] = (uint8_t)(bytes % 255u);
  memcpy(p + header, packet, bytes);
  le32(p + 22, crc(p, header + bytes));
  f->length += header + bytes;
}
int main(void) {
  fixture_t f = {.quantum = 7};
  const uint8_t head[19] = {'O','p','u','s','H','e','a','d',1,1,
                             0,0,0x80,0x3e,0,0,0,0,0};
  const uint8_t tags[16] = {'O','p','u','s','T','a','g','s',0};
  int error;
  OpusEncoder *encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_AUDIO, &error);
  assert(encoder && error == OPUS_OK);
  assert(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000)) == OPUS_OK);
  assert(opus_encoder_ctl(encoder, OPUS_SET_VBR(0)) == OPUS_OK);
  int16_t silence[320] = {0}; uint8_t packet[1275];
  int size = opus_encode(encoder, silence, 320, packet, sizeof(packet));
  assert(size == 60); opus_encoder_destroy(encoder);
  page(&f, 0, 2, 0, head, sizeof(head));
  page(&f, 1, 0, 0, tags, sizeof(tags));
  page(&f, 2, 0, 960, packet, (size_t)size);
  page(&f, 3, 4, 1920, packet, (size_t)size);
  h2_pal_mem_api_t allocator = {.user = &f, .vtable = &memory};
  h2_ogg_opus_t *reader = NULL;
  assert(h2_ogg_opus_create_packet_reader(&allocator, read_bytes, &f,
                                           512, 1275, &reader) == H2_PAL_OK);
  h2_ogg_opus_packet_t view;
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_OK && !view.bytes);
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_OK && !view.bytes);
  assert(h2_ogg_opus_headers_done(reader));
  size_t snapshot_bytes = h2_ogg_opus_checkpoint_size(reader);
  void *snapshot = malloc(snapshot_bytes); uint64_t position;
  assert(snapshot);
  assert(h2_ogg_opus_checkpoint_save(reader, snapshot, snapshot_bytes,
                                      &position) == H2_PAL_OK);
  assert(position == f.offset);
  /* Fail after two successful short reads inside a page header. Restore must
   * rewind both the parser and external byte source, not merely clear error. */
  f.fail_offset = f.offset + 14u;
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_ERR_IO && !view.bytes);
  assert(f.offset == position + 14u);
  f.offset = (size_t)position;
  assert(h2_ogg_opus_checkpoint_restore(reader, snapshot, snapshot_bytes) == H2_PAL_OK);
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_OK);
  assert(view.bytes == 60 && view.frame_samples == 320 && view.samples == 320 &&
         view.skip_samples == 0 && view.channels == 1 && view.gain_q8 == 0 &&
         !memcmp(view.data, packet, 60));
  f.offset = (size_t)position;
  assert(h2_ogg_opus_checkpoint_restore(reader, snapshot, snapshot_bytes) == H2_PAL_OK);
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_OK && view.samples == 320);
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_OK && view.samples == 320);
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_EXIT && !view.bytes);
  f.offset = 0;
  assert(h2_ogg_opus_reset_packets(reader) == H2_PAL_OK);
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_OK && !view.bytes);
  assert(f.largest < 8192); /* Independent of file duration; no decoder here. */
  f.file[28] ^= 1u; f.offset = 0;
  assert(h2_ogg_opus_reset_packets(reader) == H2_PAL_OK);
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_ERR_FORMAT);
  assert(h2_ogg_opus_checkpoint_restore(reader, snapshot, snapshot_bytes-1u) ==
         H2_PAL_ERR_INVALID_ARG);
  h2_ogg_opus_t *other = NULL;
  assert(h2_ogg_opus_create_packet_reader(&allocator, read_bytes, &f,
                                           512, 1275, &other) == H2_PAL_OK);
  assert(h2_ogg_opus_checkpoint_restore(other, snapshot, snapshot_bytes) ==
         H2_PAL_ERR_INVALID_ARG);
  h2_ogg_opus_destroy(other);
  free(snapshot); h2_ogg_opus_destroy(reader);

  /* Packet-only mode uses the exact decoder timing: half the first packet
   * is pre-skip, and EOS trims half the second one. */
  f.length = f.offset = 0;
  uint8_t trimmed[19]; memcpy(trimmed, head, sizeof(trimmed));
  trimmed[10] = 0xe0; trimmed[11] = 1; /* 480 samples at 48 kHz. */
  page(&f, 0, 2, 0, trimmed, sizeof(trimmed));
  page(&f, 1, 0, 0, tags, sizeof(tags));
  page(&f, 2, 0, 960, packet, (size_t)size);
  page(&f, 3, 4, 1440, packet, (size_t)size);
  assert(h2_ogg_opus_create_packet_reader(&allocator, read_bytes, &f,
                                           512, 1275, &reader) == H2_PAL_OK);
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_OK && view.stream_start);
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_OK && !view.bytes);
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_OK &&
         view.skip_samples == 160 && view.samples == 160);
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_OK &&
         view.skip_samples == 0 && view.samples == 160);
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_EXIT);
  h2_ogg_opus_destroy(reader);

  f.length = f.offset = 0;
  uint8_t big_tags[300] = {'O','p','u','s','T','a','g','s',0};
  page(&f, 0, 2, 0, head, sizeof(head));
  page(&f, 1, 0, 0, big_tags, sizeof(big_tags));
  assert(h2_ogg_opus_create_packet_reader(&allocator, read_bytes, &f,
                                           282, 1275, &reader) == H2_PAL_OK);
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_OK);
  assert(h2_ogg_opus_next_packet(reader, &view) == H2_PAL_ERR_NO_SPACE);
  h2_ogg_opus_destroy(reader);
  return 0;
}
