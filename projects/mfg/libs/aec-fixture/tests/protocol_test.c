#include "h2_mfg_aec_fixture.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static void check_stale_controls(void) {
  h2_mfg_aec_control_guard_t guard = {0};
  h2_mfg_aec_packet_t request = {.operation = H2_MFG_AEC_SOURCE_START,
                                 .run_hi = 123,
                                 .run_lo = 456,
                                 .sequence = 1};
  assert(h2_mfg_aec_control_accept(&guard, &request, 0));
  request.operation = H2_MFG_AEC_SOURCE_STOP;
  request.sequence = 2;
  assert(h2_mfg_aec_control_accept(&guard, &request, 1));
  request.operation = H2_MFG_AEC_SOURCE_START;
  request.sequence = 1;
  assert(!h2_mfg_aec_control_accept(&guard, &request, 0));
  assert(guard.sequence == 2);
  request.sequence = 3;
  assert(h2_mfg_aec_control_accept(&guard, &request, 0));
  request.operation = H2_MFG_AEC_SOURCE_STOP;
  request.sequence = 2;
  assert(!h2_mfg_aec_control_accept(&guard, &request, 1));
  /* Equal sequences with changed contents are not duplicate commands. */
  request.sequence = 3;
  assert(!h2_mfg_aec_control_accept(&guard, &request, 1));
  request.sequence = 4;
  assert(h2_mfg_aec_control_accept(&guard, &request, 1));
  /* Keep sequence ordering for the source-shutdown command in this run. */
  request.operation = H2_MFG_AEC_SOURCE_SHUTDOWN;
  request.sequence = 1;
  assert(!h2_mfg_aec_control_accept(&guard, &request, 0));
  request.sequence = 5;
  assert(h2_mfg_aec_control_accept(&guard, &request, 0));

  request.operation = H2_MFG_AEC_SOURCE_START;
  request.run_lo = 789;
  request.sequence = 1;
  assert(!h2_mfg_aec_control_accept(&guard, &request, 1));
  assert(h2_mfg_aec_control_accept(&guard, &request, 0));
  /* Even a higher sequence from a retired nonce cannot stop the new run. */
  request.run_lo = 456;
  request.sequence = 6;
  request.operation = H2_MFG_AEC_SOURCE_STOP;
  assert(!h2_mfg_aec_control_accept(&guard, &request, 1));
  request.sequence = 1;
  request.operation = H2_MFG_AEC_SOURCE_START;
  assert(!h2_mfg_aec_control_accept(&guard, &request, 0));
  assert(guard.run_count == 2 && guard.sequence == 1);
  request.run_lo = 789;
  request.sequence = 0;
  assert(!h2_mfg_aec_control_accept(&guard, &request, 0));
  request.sequence = UINT32_MAX;
  assert(h2_mfg_aec_control_accept(&guard, &request, 0));
  request.sequence = 1;
  assert(!h2_mfg_aec_control_accept(&guard, &request, 0));
  /* Full history fails closed instead of accepting an evicted old run. */
  for (unsigned i = 2; i < H2_MFG_AEC_MAX_RUNS; ++i) {
    request.run_hi = i;
    request.run_lo = 0;
    assert(h2_mfg_aec_control_accept(&guard, &request, 0));
  }
  request.run_hi = H2_MFG_AEC_MAX_RUNS;
  assert(!h2_mfg_aec_control_accept(&guard, &request, 0));
  request.run_hi = 123;
  request.run_lo = 456;
  assert(!h2_mfg_aec_control_accept(&guard, &request, 0));
}

int main(void) {
  check_stale_controls();
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
