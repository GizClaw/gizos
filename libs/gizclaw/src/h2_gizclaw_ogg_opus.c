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
  h2_gizclaw_ogg_opus_read_fn read;
  void *read_user;
  uint8_t *page;
  const uint8_t *laces, *body;
  size_t lace_count, lace_index, body_offset, last_complete;
  uint8_t flags;
  uint64_t granule, previous_granule, page_samples, skip;
  uint32_t serial, next_sequence;
  unsigned headers;
  bool have_stream, stream_ended, have_audio_page, page_loaded;
  uint8_t *packet;
  size_t packet_len, packet_capacity;
  /* Reader mode: an OpusTags packet past the 64 KiB packet ceiling (large
   * embedded cover art) keeps its first 64 KiB and drops the rest. */
  bool tags_truncated;
  /* Reader mode only: bytes already read past the current page (left over
   * when a resync rescans a rejected candidate) wait at page[pend_off..]. */
  size_t pend_off, pend_len;
  /* Seek: `seeking` until the first sample at or after target_g (48 kHz
   * granule domain, pre-skip included) is emitted; `landing` while a resync
   * still has to find the page its timeline is anchored to. out16 counts the
   * samples plain playback would have emitted so far, so origin16 matches
   * what a start-from-zero playback reports. */
  bool seeking, landing, reset_pending;
  uint64_t target_g, pre_skip, out16, origin16;
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

static h2_pal_result_t read_exact(h2_gizclaw_ogg_opus_t *d, uint8_t *out,
                                  size_t length, bool allow_eof) {
  size_t offset = 0;
  while (offset < length) {
    size_t count = 0;
    int rc = d->read(d->read_user, out + offset, length - offset, &count);
    if (rc == H2_PAL_EXIT)
      return offset == 0 && allow_eof ? H2_PAL_EXIT : H2_PAL_ERR_FORMAT;
    if (rc != H2_PAL_OK)
      return rc;
    if (!count || count > length - offset)
      return H2_PAL_ERR_FORMAT;
    offset += count;
  }
  return H2_PAL_OK;
}

/* The page buffer doubles as the reader's lookahead: fill() only ever reads
 * the bytes still missing, so a normal page read never overshoots. */
static h2_pal_result_t fill(h2_gizclaw_ogg_opus_t *d, size_t *have,
                            size_t need, bool allow_eof) {
  if (*have >= need)
    return H2_PAL_OK;
  int rc = read_exact(d, d->page + *have, need - *have, allow_eof && !*have);
  if (rc == H2_PAL_OK)
    *have = need;
  return rc;
}

static size_t take_pending(h2_gizclaw_ogg_opus_t *d) {
  size_t have = d->pend_len;
  if (have)
    memmove(d->page, d->page + d->pend_off, have);
  d->pend_len = 0;
  return have;
}

static void finish_page(h2_gizclaw_ogg_opus_t *d, size_t have, size_t len) {
  d->data = d->page;
  d->offset = 0;
  d->len = len;
  d->pend_off = len;
  d->pend_len = have - len;
}

static h2_pal_result_t read_page(h2_gizclaw_ogg_opus_t *d) {
  size_t have = take_pending(d);
  int rc = fill(d, &have, 27, true);
  if (rc != H2_PAL_OK)
    return rc;
  if (memcmp(d->page, "OggS", 4) != 0)
    return H2_PAL_ERR_FORMAT;
  size_t header = 27u + d->page[26];
  rc = fill(d, &have, header, false);
  if (rc != H2_PAL_OK)
    return rc;
  size_t body = 0;
  for (size_t i = 27; i < header; ++i)
    body += d->page[i];
  rc = fill(d, &have, header + body, false);
  if (rc != H2_PAL_OK)
    return rc;
  finish_page(d, have, header + body);
  return H2_PAL_OK;
}

/* Index of the first position whose bytes (up to four) match "OggS". */
static size_t capture_at(const uint8_t *p, size_t have) {
  for (size_t i = 0; i < have; ++i)
    if (!memcmp(p + i, "OggS", have - i < 4 ? have - i : 4))
      return i;
  return have;
}

/* A stream that starts at an arbitrary byte: find the next capture pattern
 * and accept only a complete page of this logical stream whose CRC matches.
 * While looking for a capture at most the four bytes that could complete it
 * are requested, so nothing past an accepted page is consumed unless it was
 * already read as part of a rejected candidate; that remainder is rescanned
 * in place and whatever follows the accepted page stays pending. */
#define SCAN_LIMIT (2u * (27u + 255u + 255u * 255u))
static h2_pal_result_t scan_page(h2_gizclaw_ogg_opus_t *d) {
  size_t have = take_pending(d), dropped = 0;
  bool eof = false;
  for (;;) {
    size_t at = capture_at(d->page, have);
    if (at) {
      memmove(d->page, d->page + at, have - at);
      have -= at;
      dropped += at;
    }
    if (dropped > SCAN_LIMIT)
      return H2_PAL_ERR_FORMAT;
    if (have < 4) {
      if (eof)
        return H2_PAL_ERR_FORMAT; /* No landable page before the end. */
      size_t count = 0;
      int rc = d->read(d->read_user, d->page + have, 4 - have, &count);
      if (rc == H2_PAL_EXIT) {
        eof = true;
        continue;
      }
      if (rc != H2_PAL_OK)
        return rc;
      if (!count || count > 4 - have)
        return H2_PAL_ERR_FORMAT;
      have += count;
      continue;
    }
    /* Fixed header, then lacing, then body; each stage only reads what the
     * previous one proved is needed. BOS and other streams are rejected. */
    const uint8_t *p = d->page;
    size_t len = 27;
    bool ok = true;
    for (unsigned stage = 0; ok && stage < 3; ++stage) {
      if (!eof) {
        int rc = fill(d, &have, len, false);
        if (rc == H2_PAL_ERR_FORMAT)
          eof = true; /* The end arrived inside this candidate. */
        else if (rc != H2_PAL_OK)
          return rc;
      }
      if (have < len)
        ok = false;
      else if (stage == 0) {
        ok = p[4] == 0 && (p[5] & ~5u) == 0 && le32(p + 14) == d->serial;
        len = 27u + p[26];
      } else if (stage == 1) {
        for (size_t i = 27; i < 27u + p[26]; ++i)
          len += p[i];
      } else
        ok = page_crc(p, len) == le32(p + 22);
    }
    if (ok) {
      finish_page(d, have, len);
      return H2_PAL_OK;
    }
    memmove(d->page, d->page + 1, have - 1);
    have -= 1;
    dropped += 1;
  }
}

/* 80 ms at 48 kHz: RFC 7845 section 4.6 pre-roll after a seek. */
#define PRE_ROLL 3840u

/* Anchor a resynced stream on the page just loaded. Returns false when the
 * page cannot anchor it: EOS (its granule may be end-trimmed), no granule, or
 * no packet that both starts and ends here. The start of the first packet
 * that begins on the page is its granule minus the durations of the packets
 * completed on it, which the TOC bytes give without decoding. */
static bool land(h2_gizclaw_ogg_opus_t *d) {
  size_t first = 0, offset = 0;
  if (d->flags & 1u) {
    while (first < d->lace_count && d->laces[first] == 255)
      offset += d->laces[first++];
    if (first == d->lace_count)
      return false;
    offset += d->laces[first++];
  }
  if ((d->flags & 4u) || d->granule == UINT64_MAX || d->granule > INT64_MAX ||
      d->last_complete == SIZE_MAX || first > d->last_complete)
    return false;
  uint64_t samples = 0;
  for (size_t i = first, at = offset; i <= d->last_complete;) {
    size_t len = 0;
    do
      len += d->laces[i];
    while (d->laces[i++] == 255);
    int n = len ? opus_packet_get_nb_samples(d->body + at, (opus_int32)len,
                                             48000)
                : 0;
    if (n <= 0 || n > 5760)
      return false;
    samples += (uint64_t)n;
    at += len;
  }
  if (samples > d->granule)
    return false;
  const uint64_t start = d->granule - samples;
  d->lace_index = first;
  d->body_offset = offset;
  d->previous_granule = start;
  d->have_audio_page = true;
  d->skip = start < d->pre_skip ? d->pre_skip - start : 0;
  const uint64_t trimmed = (d->pre_skip + 2) / 3;
  d->out16 = start / 3 > trimmed ? start / 3 - trimmed : 0;
  /* A decoder that starts mid-stream converges over the pre-roll. */
  if (start > 0 && d->target_g < start + PRE_ROLL)
    d->target_g = start + PRE_ROLL;
  d->landing = false;
  return true;
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
  for (;;) {
    if (d->read) {
      int rc = d->landing ? scan_page(d) : read_page(d);
      if (rc == H2_PAL_EXIT)
        return d->stream_ended ? H2_PAL_EXIT : H2_PAL_ERR_FORMAT;
      if (rc != H2_PAL_OK)
        return rc;
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
    if (d->landing)
      d->next_sequence = sequence; /* Numbering restarts where we landed. */
    if (!d->have_stream || d->stream_ended) {
      if (p[5] != 2 || sequence != 0 ||
          (d->have_stream && d->serial == serial))
        return H2_PAL_ERR_FORMAT;
      d->have_stream = true;
      d->stream_ended = false;
      d->serial = serial;
      d->next_sequence = 0;
      d->headers = 0;
      d->previous_granule = 0;
      d->have_audio_page = false;
      /* The new stream's OpusHead sets its own pre-skip; drop the remainder
       * of the previous stream's so it can never apply here. */
      d->skip = 0;
      /* A target inside one chained stream means nothing in the next: stop
       * at the boundary, which plain playback reaches at the same count. */
      if (d->seeking) {
        d->seeking = false;
        d->origin16 = d->out16;
      }
    } else if ((p[5] & 2u) != 0 || serial != d->serial)
      return H2_PAL_ERR_FORMAT; /* Multiplexed streams are not a playback
                                   track. */
    if (sequence != d->next_sequence++ ||
        (!d->landing && ((p[5] & 1u) != 0) != (d->packet_len != 0)))
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
    d->offset += header_len + body_len;
    if (d->landing && !land(d)) {
      d->page_loaded = false; /* Skipped whole; nothing of it is decoded. */
      continue;
    }
    if (d->headers == 0 && (d->last_complete == SIZE_MAX ||
                            d->last_complete + 1 != d->lace_count))
      return H2_PAL_ERR_FORMAT;
    d->page_loaded = true;
    return H2_PAL_OK;
  }
}

static h2_pal_result_t append(h2_gizclaw_ogg_opus_t *d, size_t len) {
  if (len > SIZE_MAX - d->packet_len)
    return H2_PAL_ERR_NO_SPACE;
  size_t needed = d->packet_len + len;
  if (d->read && needed > 65536u) {
    /* The comment header is validated, never used: past the ceiling its
     * bytes are dropped as they arrive, so memory stays one page plus
     * 64 KiB however large the tags are. Audio packets keep the limit. */
    if (d->headers != 1)
      return H2_PAL_ERR_NO_SPACE;
    d->tags_truncated = true;
    d->body_offset += len;
    return H2_PAL_OK;
  }
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
      d->skip = d->pre_skip = (uint64_t)p[10] | ((uint64_t)p[11] << 8);
      int gain = (int)p[16] | ((int)p[17] << 8);
      if (gain >= 32768)
        gain -= 65536;
      if (opus_decoder_init(d->opus, 16000, 1) != OPUS_OK ||
          opus_decoder_ctl(d->opus, OPUS_SET_GAIN(gain)) != OPUS_OK)
        return H2_PAL_ERR_IO;
    } else if (d->tags_truncated ? memcmp(p, "OpusTags", 8) != 0
                                 : !valid_tags(p, len))
      return H2_PAL_ERR_FORMAT; /* A truncated one can only show its magic. */
    d->tags_truncated = false;
    ++d->headers;
    return H2_PAL_OK;
  }
  if (len == 0 || len > INT32_MAX)
    return H2_PAL_ERR_FORMAT;
  int samples48 = opus_packet_get_nb_samples(p, (opus_int32)len, 48000);
  if (samples48 <= 0 || samples48 > 5760)
    return H2_PAL_ERR_FORMAT;
  const uint64_t before = d->page_samples;
  /* Granule-domain start of this packet; exact once the page is anchored. */
  const uint64_t start = d->previous_granule + before;
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
  const size_t count = (size_t)samples48 / 3;
  size_t trim = (size_t)((d->skip + 2) / 3);
  if (trim > count)
    trim = count;
  d->skip = d->skip > (uint64_t)samples48 ? d->skip - (uint64_t)samples48 : 0;
  const size_t end = (size_t)(keep / 3);
  const uint64_t emitted = end > trim ? end - trim : 0;
  /* Seeking: a packet that ends before the pre-roll window is validated and
   * counted but never decoded; the decoder state is reset before the first
   * packet that is, and samples before the target are discarded. */
  if (d->seeking &&
      start + (uint64_t)samples48 + PRE_ROLL <= d->target_g) {
    d->out16 += emitted;
    d->reset_pending = true;
    return H2_PAL_OK;
  }
  if (d->reset_pending) {
    if (opus_decoder_ctl(d->opus, OPUS_RESET_STATE) != OPUS_OK)
      return H2_PAL_ERR_IO;
    d->reset_pending = false;
  }
  int decoded = opus_decode(d->opus, p, (opus_int32)len, d->samples, 1920, 0);
  if (decoded < 0 || (size_t)decoded != count)
    return H2_PAL_ERR_FORMAT;
  size_t first = trim;
  if (d->seeking) {
    uint64_t late = d->target_g > start ? (d->target_g - start + 2) / 3 : 0;
    if (late > first)
      first = late > count ? count : (size_t)late;
    if (first < end) {
      d->origin16 = d->out16 + (first - trim);
      d->seeking = false;
    }
  }
  d->out16 += emitted;
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
      if (rc == H2_PAL_EXIT && d->seeking && !d->landing) {
        /* Validated end before the target: the track is over there. */
        d->seeking = false;
        d->origin16 = d->out16;
      }
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

bool h2_gizclaw_ogg_opus_headers_done(const h2_gizclaw_ogg_opus_t *d) {
  return d != NULL && d->error == H2_PAL_OK && d->headers == 2 &&
         !d->have_audio_page && !d->stream_ended;
}

h2_pal_result_t h2_gizclaw_ogg_opus_seek(h2_gizclaw_ogg_opus_t *d,
                                         uint64_t target_ms, bool resync) {
  if (d == NULL || (resync && d->read == NULL) ||
      target_ms > (UINT64_MAX - 65535u - PRE_ROLL) / 48u)
    return H2_PAL_ERR_INVALID_ARG;
  if (!h2_gizclaw_ogg_opus_headers_done(d) || d->seeking)
    return H2_PAL_ERR_INVALID_STATE;
  d->target_g = target_ms * 48u + d->pre_skip;
  d->seeking = true;
  d->landing = resync;
  d->pend_len = 0;
  return H2_PAL_OK;
}

bool h2_gizclaw_ogg_opus_origin(const h2_gizclaw_ogg_opus_t *d,
                                uint64_t *samples) {
  if (d == NULL || samples == NULL || d->seeking)
    return false;
  *samples = d->origin16;
  return true;
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

h2_pal_result_t
h2_gizclaw_ogg_opus_create_reader(const h2_pal_mem_api_t *allocator,
                                  h2_gizclaw_ogg_opus_read_fn read, void *user,
                                  h2_gizclaw_ogg_opus_t **out) {
  if (out)
    *out = NULL;
  if (!read)
    return H2_PAL_ERR_INVALID_ARG;
  static const uint8_t empty = 0;
  int rc = h2_gizclaw_ogg_opus_create(allocator, &empty, 1, out);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_ogg_opus_t *d = *out;
  d->read = read;
  d->read_user = user;
  d->page = h2_pal_mem_alloc(allocator, 27u + 255u + 255u * 255u);
  if (!d->page) {
    h2_gizclaw_ogg_opus_destroy(d);
    *out = NULL;
    return H2_PAL_ERR_NO_MEMORY;
  }
  return H2_PAL_OK;
}

void h2_gizclaw_ogg_opus_destroy(h2_gizclaw_ogg_opus_t *d) {
  if (d == NULL)
    return;
  h2_pal_mem_free(d->allocator, d->opus);
  h2_pal_mem_free(d->allocator, d->packet);
  h2_pal_mem_free(d->allocator, d->page);
  h2_pal_mem_free(d->allocator, d);
}
