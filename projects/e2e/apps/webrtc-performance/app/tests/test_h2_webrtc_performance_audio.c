/* Private callback contract only; these synthetic packets are not E2E receipts. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/h2_webrtc_performance.c"

#include <assert.h>

typedef struct test_clock {
  uint64_t now_us;
  unsigned reads;
} test_clock_t;

static h2_pal_result_t clock_us(void *user, uint64_t *out_us) {
  test_clock_t *clock = user;
  ++clock->reads;
  *out_us = clock->now_us;
  return H2_PAL_OK;
}

int main(void) {
  test_clock_t clock = {.now_us = 20000u};
  const h2_pal_time_vtable_t time_vtable = {.get_monotonic_us = clock_us};
  const h2_pal_time_api_t time_api = {.user = &clock, .vtable = &time_vtable};
  h2_runtime_t runtime = {.time = &time_api};
  h2_webrtc_performance_state_t state = {.runtime = &runtime};
  uint8_t frame[10] = {0};

  h2_webrtc_perf_on_opus(&state, NULL, NULL, 0u);
  h2_webrtc_perf_on_opus(&state, NULL, frame, 0u);
  assert(state.callback_error == 0 && state.audio_received == 0u);
  assert(state.audio_next_sequence == 0u && state.audio_last_arrival_ns == 0u);
  assert(clock.reads == 0u);

  h2_webrtc_perf_on_opus(&state, NULL, frame, sizeof(frame));
  assert(state.audio_received == 1u && state.audio_next_sequence == 1u);
  const uint64_t previous = state.audio_last_arrival_ns;
  const unsigned reads = clock.reads;
  h2_webrtc_perf_on_opus(&state, NULL, NULL, 0u);
  assert(state.audio_received == 1u && state.audio_last_arrival_ns == previous);
  assert(clock.reads == reads);
  clock.now_us = 60000u;
  h2_webrtc_perf_write_u16(frame, 2u);
  h2_webrtc_perf_on_opus(&state, NULL, frame, sizeof(frame));
  assert(state.callback_error == 0 && state.audio_received == 2u);
  assert(state.audio_next_sequence == 3u && state.audio_seen[0] == 5u);
  assert(state.audio_gap_ns[1] == UINT64_C(40000000));
  /* Missing sequence1 stays absent; an erasure does not counterfeit receipt. */
  assert((state.audio_seen[0] & 2u) == 0u);

  state = (h2_webrtc_performance_state_t){.runtime = &runtime};
  h2_webrtc_perf_on_opus(&state, NULL, NULL, sizeof(frame));
  assert(state.callback_error == 1 && state.callback_error_reason == 11);
  assert(state.audio_received == 0u);
  state = (h2_webrtc_performance_state_t){.runtime = &runtime};
  h2_webrtc_perf_on_opus(&state, NULL, frame, sizeof(frame) - 1u);
  assert(state.callback_error == 1 && state.callback_error_reason == 11);
  state = (h2_webrtc_performance_state_t){.runtime = &runtime};
  h2_webrtc_perf_write_u16(frame, H2_WEBRTC_PERF_AUDIO_FRAMES);
  h2_webrtc_perf_on_opus(&state, NULL, frame, sizeof(frame));
  assert(state.callback_error == 1 && state.callback_error_reason == 12);
  state = (h2_webrtc_performance_state_t){.runtime = &runtime};
  h2_webrtc_perf_write_u16(frame, 0u);
  h2_webrtc_perf_write_u64(frame + 2u, clock.now_us * 1000u + 1u);
  h2_webrtc_perf_on_opus(&state, NULL, frame, sizeof(frame));
  assert(state.callback_error == 1 && state.callback_error_reason == 13);
  assert(state.audio_received == 0u);
  return 0;
}
