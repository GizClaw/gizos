#include <assert.h>
#include <stdio.h>
static unsigned synchronous_logs;
static int counted_printf(const char* format, ...) {
  (void)format;
  ++synchronous_logs;
  return 0;
}
#define printf counted_printf
#include "../src/h2_webrtc_performance.c"
#undef printf

int main(void) {
  h2_runtime_t runtime = {0};
  h2_webrtc_performance_state_t state = {.runtime = &runtime};
  uint8_t frame[10] = {0};
  h2_webrtc_perf_on_opus(&state, NULL, NULL, 0u);
  h2_webrtc_perf_on_opus(&state, NULL, frame, 0u);
  assert(!state.callback_error && state.audio_loss_markers == 2u);
  assert(state.audio_received == 0u && state.audio_next_sequence == 0u);
  h2_webrtc_perf_on_opus(&state, NULL, frame, sizeof(frame));
  h2_webrtc_perf_on_opus(&state, NULL, frame, sizeof(frame));
  assert(state.audio_received == 1u && state.audio_duplicates == 1u);
  assert(!state.callback_error);
  h2_webrtc_perf_on_opus(&state, NULL, NULL, 0u);
  h2_webrtc_perf_write_u16(frame, 2u);
  h2_webrtc_perf_on_opus(&state, NULL, frame, sizeof(frame));
  assert(!state.callback_error && state.audio_received == 2u);
  assert(state.audio_next_sequence == 3u && state.audio_loss_markers == 3u);
  h2_webrtc_perf_write_u16(frame, 1u);
  h2_webrtc_perf_on_opus(&state, NULL, frame, sizeof(frame));
  assert(!state.callback_error && state.audio_received == 3u);
  assert(state.audio_reordered == 1u && state.audio_loss_markers == 3u);
  /* Loss and reordering must be counted without blocking on the console. */
  assert(synchronous_logs == 0u);

  /* Nonempty malformed payloads remain errors. */
  state.callback_error = 0;
  h2_webrtc_perf_on_opus(&state, NULL, NULL, 10u);
  assert(state.callback_error);
  state.callback_error = 0;
  h2_webrtc_perf_on_opus(&state, NULL, frame, 9u);
  assert(state.callback_error);
  state.callback_error = 0;
  h2_webrtc_perf_write_u16(frame, H2_WEBRTC_PERF_AUDIO_FRAMES);
  h2_webrtc_perf_on_opus(&state, NULL, frame, sizeof(frame));
  assert(state.callback_error);
  state.callback_error = 0;
  h2_webrtc_perf_write_u16(frame, 3u);
  h2_webrtc_perf_write_u64(frame + 2u, 1u);
  h2_webrtc_perf_on_opus(&state, NULL, frame, sizeof(frame));
  assert(state.callback_error); /* Future timestamp with the zero test clock. */
  return 0;
}
