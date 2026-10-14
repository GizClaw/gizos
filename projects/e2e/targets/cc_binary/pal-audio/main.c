#include "h2_desktop_application_host.h"
#include "h2_pal_audio_e2e.h"
#include "h2_portaudio.h"
#include "h2_atomic.h"

#include <stdio.h>

typedef struct audio_witness {
  h2_atomic_uint_t mic_frames;
  h2_atomic_uint_t speaker_frames;
  h2_atomic_uint_t mic_peak;
  h2_atomic_uint_t speaker_peak;
} audio_witness_t;

static void observe(void *user, const h2_portaudio_capture_frame_t *frame,
                    int mic) {
  audio_witness_t *witness = user;
  h2_atomic_uint_t *count = mic ? &witness->mic_frames : &witness->speaker_frames;
  h2_atomic_uint_t *peak = mic ? &witness->mic_peak : &witness->speaker_peak;
  h2_atomic_fetch_add(count, 1u);
  unsigned maximum = 0u;
  for (size_t i = 0u; i < frame->frames * frame->channels; ++i) {
    const int32_t sample = frame->samples[i];
    const unsigned value = (unsigned)(sample < 0 ? -sample : sample);
    if (value > maximum) maximum = value;
  }
  unsigned current = h2_atomic_load(peak);
  while (current < maximum &&
         !h2_atomic_compare_exchange_strong(peak, &current, maximum)) {}
}
static void on_mic(void *user, const h2_portaudio_capture_frame_t *frame) {
  observe(user, frame, 1);
}
static void on_speaker(void *user, const h2_portaudio_capture_frame_t *frame) {
  observe(user, frame, 0);
}
static void report(void *user, const h2_pal_audio_e2e_case_result_t *item) {
  (void)user;
  printf("H2_PAL_AUDIO_CASE {\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u}\n",
         item->id, item->passed ? "PASS" : item->blocked ? "BLOCKED" : "FAIL",
         item->detail, item->line);
  fflush(stdout);
}
static h2_pal_audio_e2e_result_t result;
static audio_witness_t witness;
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)host; (void)user;
  const h2_pal_audio_e2e_config_t config = {
      .audio = runtime->audio, .time = runtime->time, .stability_ms = 30000u, .report = report};
  int rc = h2_pal_audio_e2e_run(&config, &result);
  {
    printf("H2_PAL_AUDIO_SUMMARY {\"passed\":%u,\"failed\":%u,\"blocked\":%u,"
           "\"mic_frames\":%u,\"mic_peak\":%u,\"mic_energy\":%llu,"
           "\"speaker_frames\":%u,\"output_peak\":%u,"
           "\"witness_mic_frames\":%u,\"witness_speaker_frames\":%u,"
           "\"witness_mic_peak\":%u,\"witness_speaker_peak\":%u,"
           "\"stability_elapsed_ms\":%llu}\n",
           result.passed, result.failed, result.blocked,
           result.mic_frames, result.mic_peak,
           (unsigned long long)result.mic_energy,
           result.speaker_frames, result.output_peak,
           h2_atomic_load(&witness.mic_frames),
           h2_atomic_load(&witness.speaker_frames),
           h2_atomic_load(&witness.mic_peak),
           h2_atomic_load(&witness.speaker_peak),
           (unsigned long long)result.stability_elapsed_ms);
  }
  return rc;
}
int main(void) {
  if (h2_atomic_init(&witness.mic_frames, 0u) != H2_ATOMIC_OK ||
      h2_atomic_init(&witness.speaker_frames, 0u) != H2_ATOMIC_OK ||
      h2_atomic_init(&witness.mic_peak, 0u) != H2_ATOMIC_OK ||
      h2_atomic_init(&witness.speaker_peak, 0u) != H2_ATOMIC_OK) {
    return 1;
  }
  const h2_desktop_application_host_config_t platform = {
      .services = H2_DESKTOP_HOST_AUDIO, .require_real_audio = 1,
      .audio_capture = {.user = &witness, .on_mic = on_mic, .on_speaker = on_speaker}};
  const h2_application_host_config_t app = {.entry = run};
  h2_application_host_result_t lifecycle = {0};
  int rc = h2_desktop_application_host_run(&platform, &app, &lifecycle);
  if (lifecycle.retained) return 1;
  const int witnessed = h2_atomic_load(&witness.mic_frames) > 0u &&
                        h2_atomic_load(&witness.speaker_frames) > 0u &&
                        h2_atomic_load(&witness.speaker_peak) > 0u;
  h2_atomic_destroy(&witness.mic_frames);
  h2_atomic_destroy(&witness.speaker_frames);
  h2_atomic_destroy(&witness.mic_peak);
  h2_atomic_destroy(&witness.speaker_peak);
  return rc == H2_AUDIO_OK && lifecycle.cleanup == H2_PAL_OK && witnessed &&
                 result.stability_elapsed_ms >= 30000u ? 0 : 1;
}
