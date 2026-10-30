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
  H2_MFG_AEC_PROBE_SAMPLES = 512,
  H2_MFG_AEC_MAX_RUNS = 256
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

typedef struct h2_mfg_aec_control_guard {
  uint64_t runs[H2_MFG_AEC_MAX_RUNS];
  size_t run_count;
  uint32_t sequence;
} h2_mfg_aec_control_guard_t;

/* Call only for a validated request which is not the exact cached duplicate.
 * A run uses a unique nonce and increasing, nonzero sequences. Keep retired
 * nonces for the peer's whole lifetime: switching runs must not make delayed
 * START/STOP packets current again. Fail closed when history is full; never
 * evict replay protection. A new run starts at sequence 1 with audio stopped.
 */
static inline int h2_mfg_aec_control_accept(h2_mfg_aec_control_guard_t* guard,
                                            const h2_mfg_aec_packet_t* request,
                                            int source_active) {
  uint64_t run = ((uint64_t)request->run_hi << 32) | request->run_lo;
  if (guard->run_count != 0 && guard->runs[guard->run_count - 1] == run) {
    if (request->sequence <= guard->sequence) return 0;
  } else {
    if (source_active || request->sequence != 1 ||
        guard->run_count == H2_MFG_AEC_MAX_RUNS)
      return 0;
    for (size_t i = 0; i < guard->run_count; ++i)
      if (guard->runs[i] == run) return 0;
    guard->runs[guard->run_count++] = run;
  }
  guard->sequence = request->sequence;
  return 1;
}

#endif
