#include "h2_mfg_aec_fixture.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

int main(void) {
  h2_mfg_aec_packet_t p = {.operation = H2_MFG_AEC_SOURCE_START,
                           .sequence = 3,
                           .run_hi = 456,
                           .run_lo = 789,
                           .samples = 512,
                           .sample_rate = 16000};
  memcpy(p.source_uid, "b81f3fcad4fa", 12);
  memcpy(p.target_uid, "30eda0ae0f86", 12);
  for (unsigned i = 0; i < p.samples; ++i) p.pcm[i] = (int16_t)(i * 11);
  h2_mfg_aec_packet_seal(&p);
  assert(h2_mfg_aec_packet_valid(&p, h2_mfg_aec_packet_size(&p)));
  assert(!h2_mfg_aec_packet_valid(&p, sizeof(p) - 1));
  p.pcm[511] ^= 1;
  assert(!h2_mfg_aec_packet_valid(&p, sizeof(p)));
  p.pcm[511] ^= 1;
  p.run_hi ^= 1;
  assert(!h2_mfg_aec_packet_valid(&p, sizeof(p)));
  p.run_hi ^= 1;
  p.samples = 513;
  assert(!h2_mfg_aec_packet_valid(&p, sizeof(p)));
  p.samples = 0;
  p.operation = H2_MFG_AEC_SOURCE_ACK;
  h2_mfg_aec_packet_seal(&p);
  assert(h2_mfg_aec_packet_valid(&p, 60));
  assert(!h2_mfg_aec_packet_valid(&p, 59));
  return 0;
}
