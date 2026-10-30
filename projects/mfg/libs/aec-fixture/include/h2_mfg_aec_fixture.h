#ifndef H2_MFG_AEC_FIXTURE_H
#define H2_MFG_AEC_FIXTURE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ESP-to-ESP acoustic fixture, version 1, little-endian header/PCM. No host
 * audio injection. One datagram carries the exact mono probe supplied by the
 * existing calibration runner. Responses bind nonce, sequence, both UIDs
 * and checksum; commands become successful only after real source control. */
enum {
  H2_MFG_AEC_PORT = 18082,
  H2_MFG_AEC_FIXTURE_MAGIC = 0x31434541,
  H2_MFG_AEC_SOURCE_STOP = 0,
  H2_MFG_AEC_SOURCE_START = 1,
  H2_MFG_AEC_SOURCE_SHUTDOWN = 2,
  H2_MFG_AEC_SOURCE_ACK = 0x80,
  H2_MFG_AEC_PROBE_SAMPLES = 512
};

typedef struct h2_mfg_aec_packet {
  uint32_t magic, operation, run_lo, run_hi, sequence, sample_rate, samples;
  int32_t result;
  char source_uid[12], target_uid[12];
  uint32_t checksum;
  int16_t pcm[H2_MFG_AEC_PROBE_SAMPLES];
} h2_mfg_aec_packet_t;

_Static_assert(offsetof(h2_mfg_aec_packet_t, pcm) == 60,
               "fixture wire header layout");

static inline size_t h2_mfg_aec_packet_size(const h2_mfg_aec_packet_t* p) {
  return offsetof(h2_mfg_aec_packet_t, pcm) +
         (size_t)p->samples * sizeof(int16_t);
}

static inline uint32_t h2_mfg_aec_packet_checksum(
    const h2_mfg_aec_packet_t* p) {
  const uint8_t* bytes = (const uint8_t*)p;
  uint32_t hash = 2166136261u;
  size_t length = h2_mfg_aec_packet_size(p);
  if (p->samples > H2_MFG_AEC_PROBE_SAMPLES) return 0;
  for (size_t i = 0; i < length; ++i) {
    if (i >= offsetof(h2_mfg_aec_packet_t, checksum) &&
        i < offsetof(h2_mfg_aec_packet_t, pcm))
      continue;
    hash = (hash ^ bytes[i]) * 16777619u;
  }
  return hash;
}

static inline int h2_mfg_aec_packet_valid(const h2_mfg_aec_packet_t* p,
                                          size_t bytes) {
  return bytes >= offsetof(h2_mfg_aec_packet_t, pcm) &&
         p->magic == H2_MFG_AEC_FIXTURE_MAGIC &&
         p->samples <= H2_MFG_AEC_PROBE_SAMPLES &&
         bytes == h2_mfg_aec_packet_size(p) &&
         p->checksum == h2_mfg_aec_packet_checksum(p);
}

static inline void h2_mfg_aec_packet_seal(h2_mfg_aec_packet_t* p) {
  p->magic = H2_MFG_AEC_FIXTURE_MAGIC;
  p->checksum = h2_mfg_aec_packet_checksum(p);
}

#endif
