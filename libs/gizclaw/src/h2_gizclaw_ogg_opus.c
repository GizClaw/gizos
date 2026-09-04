#include "h2_gizclaw_ogg_opus_internal.h"

#include "opus.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

/* Container checks follow RFC 3533; header placement, sample positions and
 * trimming follow RFC 7845. No recovery/concealment for corrupt stored audio.
 */
struct h2_gizclaw_ogg_opus {
  const h2_pal_mem_api_t *allocator;
  const uint8_t *data;
  size_t len, offset;
  const uint8_t *laces, *body;
  size_t lace_count, lace_index, body_offset, last_complete;
  uint8_t flags;
  uint64_t granule, previous_granule, page_samples, skip;
  uint32_t serial, next_sequence;
  unsigned headers;
  bool have_stream, stream_ended, have_audio_page, page_loaded;
  uint8_t *packet;
  size_t packet_len, packet_capacity;
  OpusDecoder *opus;
  opus_int16 samples[1920];
  h2_pal_result_t error;
};

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p) {
  return le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static uint32_t page_crc(const uint8_t *p, size_t len) {
  uint32_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint32_t)((i >= 22 && i < 26) ? 0 : p[i]) << 24;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0);
  }
  return crc;
}

static h2_pal_result_t load_page(h2_gizclaw_ogg_opus_t *d) {
  if (d->page_loaded) {
    if (d->last_complete == SIZE_MAX && d->granule != UINT64_MAX)
      return H2_PAL_ERR_FORMAT;
    if (d->flags & 4u) {
      if (d->packet_len != 0 || d->page_samples == 0 || d->headers != 2)
        return H2_PAL_ERR_FORMAT;
      d->stream_ended = true;
    }
  }
  if (d->offset == d->len)
    return d->stream_ended ? H2_PAL_EXIT : H2_PAL_ERR_FORMAT;
  const uint8_t *p = d->data + d->offset;
  const size_t available = d->len - d->offset;
  if (available < 27 || memcmp(p, "OggS", 4) != 0 || p[4] != 0 ||
      (p[5] & ~7u) != 0)
    return H2_PAL_ERR_FORMAT;
  size_t header_len = 27u + p[26], body_len = 0;
  if (available < header_len)
    return H2_PAL_ERR_FORMAT;
  for (size_t i = 27; i < header_len; ++i)
    body_len += p[i];
  if (body_len > available - header_len ||
      page_crc(p, header_len + body_len) != le32(p + 22))
    return H2_PAL_ERR_FORMAT;
  uint32_t serial = le32(p + 14), sequence = le32(p + 18);
  if (!d->have_stream || d->stream_ended) {
    if (p[5] != 2 || sequence != 0 || (d->have_stream && d->serial == serial))
      return H2_PAL_ERR_FORMAT;
    d->have_stream = true;
    d->stream_ended = false;
    d->serial = serial;
    d->next_sequence = 0;
    d->headers = 0;
    d->previous_granule = 0;
    d->have_audio_page = false;
  } else if ((p[5] & 2u) != 0 || serial != d->serial)
    return H2_PAL_ERR_FORMAT; /* Multiplexed streams are not a playback track.
                               */
  if (sequence != d->next_sequence++ ||
      ((p[5] & 1u) != 0) != (d->packet_len != 0))
    return H2_PAL_ERR_FORMAT;
  d->flags = p[5];
  d->granule = le64(p + 6);
  d->laces = p + 27;
  d->lace_count = p[26];
  d->lace_index = d->body_offset = 0;
  d->body = p + header_len;
  d->page_samples = 0;
  d->last_complete = SIZE_MAX;
  for (size_t i = 0; i < d->lace_count; ++i)
    if (d->laces[i] < 255)
      d->last_complete = i;
  if (d->headers == 0 &&
      (d->last_complete == SIZE_MAX || d->last_complete + 1 != d->lace_count))
    return H2_PAL_ERR_FORMAT;
  d->page_loaded = true;
  d->offset += header_len + body_len;
  return H2_PAL_OK;
}

static h2_pal_result_t append(h2_gizclaw_ogg_opus_t *d, size_t len) {
  if (len > SIZE_MAX - d->packet_len)
    return H2_PAL_ERR_NO_SPACE;
  size_t needed = d->packet_len + len;
  if (needed > d->packet_capacity) {
    size_t capacity = d->packet_capacity ? d->packet_capacity : 512;
    while (capacity < needed) {
      if (capacity > SIZE_MAX / 2) {
        capacity = needed;
        break;
      }
      capacity *= 2;
    }
    /* Allocate/copy rather than require optional PAL realloc support. */
    uint8_t *packet = h2_pal_mem_alloc(d->allocator, capacity);
    if (packet == NULL)
      return H2_PAL_ERR_NO_MEMORY;
    if (d->packet_len != 0)
      memcpy(packet, d->packet, d->packet_len);
    h2_pal_mem_free(d->allocator, d->packet);
    d->packet = packet;
    d->packet_capacity = capacity;
  }
  if (len != 0)
    memcpy(d->packet + d->packet_len, d->body + d->body_offset, len);
  d->packet_len = needed;
  d->body_offset += len;
  return H2_PAL_OK;
}

static bool valid_tags(const uint8_t *p, size_t len) {
  if (len < 16 || memcmp(p, "OpusTags", 8) != 0)
    return false;
  size_t vendor_len = le32(p + 8);
  if (vendor_len > len - 16)
    return false;
  size_t offset = 12 + vendor_len;
  uint32_t count = le32(p + offset);
  offset += 4;
  if (count > (len - offset) / 4)
    return false;
  for (uint32_t i = 0; i < count; ++i) {
    if (len - offset < 4)
      return false;
    size_t comment_len = le32(p + offset);
    offset += 4;
    if (comment_len > len - offset)
      return false;
    offset += comment_len;
  }
  return true; /* RFC 7845 permits trailing padding/extension data. */
}

static h2_pal_result_t decode_packet(h2_gizclaw_ogg_opus_t *d, uint8_t *pcm,
                                     size_t *out_len) {
  const uint8_t *p = d->packet;
  const size_t len = d->packet_len;
  if (d->headers < 2) {
    if (d->lace_index != d->lace_count || d->granule != 0 || (d->flags & 4u))
      return H2_PAL_ERR_FORMAT;
    if (d->headers == 0) {
      if (len < 19 || memcmp(p, "OpusHead", 8) != 0 || p[8] == 0 || p[8] > 15)
        return H2_PAL_ERR_FORMAT;
      if (p[18] != 0 || (p[9] != 1 && p[9] != 2))
        return H2_PAL_ERR_UNSUPPORTED;
      if (p[8] == 1 && len != 19)
        return H2_PAL_ERR_FORMAT;
      d->skip = (uint64_t)p[10] | ((uint64_t)p[11] << 8);
      int gain = (int)p[16] | ((int)p[17] << 8);
      if (gain >= 32768)
        gain -= 65536;
      if (opus_decoder_init(d->opus, 16000, 1) != OPUS_OK ||
          opus_decoder_ctl(d->opus, OPUS_SET_GAIN(gain)) != OPUS_OK)
        return H2_PAL_ERR_IO;
    } else if (!valid_tags(p, len))
      return H2_PAL_ERR_FORMAT;
    ++d->headers;
    return H2_PAL_OK;
  }
  if (len == 0 || len > INT32_MAX)
    return H2_PAL_ERR_FORMAT;
  int samples48 = opus_packet_get_nb_samples(p, (opus_int32)len, 48000);
  if (samples48 <= 0 || samples48 > 5760)
    return H2_PAL_ERR_FORMAT;
  const uint64_t before = d->page_samples;
  d->page_samples += (uint64_t)samples48;
  uint64_t keep = (uint64_t)samples48;
  if (d->flags & 4u) {
    if (d->granule == UINT64_MAX || d->granule < d->previous_granule)
      return H2_PAL_ERR_FORMAT;
    uint64_t budget = d->granule - d->previous_granule;
    if (!d->have_audio_page && budget < d->skip)
      return H2_PAL_ERR_FORMAT;
    keep = budget <= before ? 0 : budget - before;
    if (keep > (uint64_t)samples48)
      keep = (uint64_t)samples48;
  }
  if (d->lace_index - 1 == d->last_complete) {
    if (d->granule == UINT64_MAX || d->granule > INT64_MAX ||
        ((d->flags & 4u) && d->have_audio_page &&
         d->granule - d->previous_granule > d->page_samples) ||
        (!(d->flags & 4u) &&
         (d->have_audio_page
              ? d->granule - d->previous_granule != d->page_samples
              : d->granule < d->page_samples)))
      return H2_PAL_ERR_FORMAT;
    d->previous_granule = d->granule;
    d->have_audio_page = true;
  }
  int decoded = opus_decode(d->opus, p, (opus_int32)len, d->samples, 1920, 0);
  if (decoded < 0 || decoded != samples48 / 3)
    return H2_PAL_ERR_FORMAT;
  size_t first = (size_t)((d->skip + 2) / 3);
  if (first > (size_t)decoded)
    first = (size_t)decoded;
  d->skip = d->skip > (uint64_t)samples48 ? d->skip - (uint64_t)samples48 : 0;
  size_t end = (size_t)(keep / 3);
  for (size_t i = first; i < end; ++i) {
    uint16_t sample = (uint16_t)d->samples[i];
    pcm[(*out_len)++] = (uint8_t)sample;
    pcm[(*out_len)++] = (uint8_t)(sample >> 8);
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_ogg_opus_next(h2_gizclaw_ogg_opus_t *d, uint8_t *pcm,
                                         size_t capacity, size_t *out_len) {
  if (out_len != NULL)
    *out_len = 0;
  if (d == NULL || pcm == NULL || out_len == NULL ||
      capacity < H2_GIZCLAW_OGG_OPUS_PCM_BYTES)
    return H2_PAL_ERR_INVALID_ARG;
  if (d->error != H2_PAL_OK)
    return d->error;
  for (;;) {
    if (!d->page_loaded || d->lace_index == d->lace_count) {
      h2_pal_result_t rc = load_page(d);
      if (rc != H2_PAL_OK)
        return d->error = rc;
    }
    if (d->lace_count == 0)
      return H2_PAL_OK;
    size_t len = d->laces[d->lace_index++];
    h2_pal_result_t rc = append(d, len);
    if (rc != H2_PAL_OK)
      return d->error = rc;
    if (len == 255 && d->lace_index == d->lace_count)
      return H2_PAL_OK; /* Yield between pages of a large continued packet. */
    if (len == 255)
      continue;
    rc = decode_packet(d, pcm, out_len);
    d->packet_len = 0;
    if (rc != H2_PAL_OK)
      d->error = rc;
    return rc;
  }
}

h2_pal_result_t h2_gizclaw_ogg_opus_create(const h2_pal_mem_api_t *allocator,
                                           const uint8_t *data, size_t len,
                                           h2_gizclaw_ogg_opus_t **out) {
  if (out != NULL)
    *out = NULL;
  if (out == NULL || data == NULL || len == 0 || allocator == NULL ||
      allocator->vtable == NULL || allocator->vtable->alloc == NULL ||
      allocator->vtable->free == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_ogg_opus_t *d = h2_pal_mem_alloc(allocator, sizeof(*d));
  if (d == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(d, 0, sizeof(*d));
  d->allocator = allocator;
  d->data = data;
  d->len = len;
  int size = opus_decoder_get_size(1);
  d->opus = size > 0 ? h2_pal_mem_alloc(allocator, (size_t)size) : NULL;
  if (d->opus == NULL) {
    h2_gizclaw_ogg_opus_destroy(d);
    return H2_PAL_ERR_NO_MEMORY;
  }
  *out = d;
  return H2_PAL_OK;
}

void h2_gizclaw_ogg_opus_destroy(h2_gizclaw_ogg_opus_t *d) {
  if (d == NULL)
    return;
  h2_pal_mem_free(d->allocator, d->opus);
  h2_pal_mem_free(d->allocator, d->packet);
  h2_pal_mem_free(d->allocator, d);
}
