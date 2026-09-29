#include "mobile_runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct allocator_probe {
  unsigned attempts;
  unsigned allocations;
  unsigned frees;
  unsigned live;
  int fail;
  int failure_rejected;
  int passed;
} allocator_probe_t;

static allocator_probe_t allocator_probe;

static void *probe_alloc(void *user, size_t bytes) {
  allocator_probe_t *probe = user;
  ++probe->attempts;
  void *ptr = probe->fail ? NULL : malloc(bytes);
  if (ptr != NULL) { ++probe->allocations; ++probe->live; }
  return ptr;
}

static void *probe_realloc(void *user, void *ptr, size_t bytes) {
  allocator_probe_t *probe = user;
  if (ptr == NULL) return probe_alloc(user, bytes);
  if (probe->fail || bytes == 0u) return NULL;
  return realloc(ptr, bytes);
}

static void probe_free(void *user, void *ptr) {
  allocator_probe_t *probe = user;
  if (ptr != NULL) { ++probe->frees; --probe->live; free(ptr); }
}

/* Execute against each packaged mobile provider: an ignored allocator must
 * fail this probe, and every successful caller-owned allocation must return. */
static int probe_track_allocator(const h2_pal_audio_api_t *audio) {
  memset(&allocator_probe, 0, sizeof(allocator_probe));
  h2_audio_info_t info;
  int rc = h2_pal_audio_get_info(audio, &info);
  if (rc != H2_AUDIO_OK) return rc;
  rc = h2_pal_audio_start_speaker(audio);
  if (rc != H2_AUDIO_OK) return rc;
  const h2_pal_mem_vtable_t vtable = {
      .alloc = probe_alloc, .realloc = probe_realloc, .free = probe_free};
  const h2_pal_mem_api_t allocator = {
      .user = &allocator_probe, .vtable = &vtable};
  h2_audio_track_config_t config = {
      .name = "pal-audio-allocator", .format = info.playback_format,
      .volume_factor_milli = 300u, .buffer_frames = 4u,
      .allocator = &allocator};
  h2_pal_audio_track_t *track = NULL;
  allocator_probe.fail = 1;
  rc = h2_pal_audio_create_track(audio, &config, &track);
  allocator_probe.failure_rejected = rc == H2_AUDIO_ERR_NO_MEMORY &&
      track == NULL && allocator_probe.attempts != 0u && allocator_probe.live == 0u;
  if (!allocator_probe.failure_rejected) rc = H2_AUDIO_ERR_INVALID_STATE;
  else {
    allocator_probe.fail = 0;
    rc = h2_pal_audio_create_track(audio, &config, &track);
    if (rc == H2_AUDIO_OK &&
        (track == NULL || allocator_probe.allocations == 0u || allocator_probe.live == 0u))
      rc = H2_AUDIO_ERR_INVALID_STATE;
    int16_t samples[4096] = {1};
    h2_audio_frame_t frame = h2_audio_frame_for_buffer(
        samples, sizeof(samples), info.playback_format);
    frame.bytes = (size_t)frame.samples_per_channel * frame.channels * sizeof(int16_t);
    if (rc == H2_AUDIO_OK && frame.bytes > sizeof(samples))
      rc = H2_AUDIO_ERR_INVALID_ARG;
    if (rc == H2_AUDIO_OK) rc = h2_pal_audio_track_write(track, &frame, 3000u);
    if (rc == H2_AUDIO_OK) rc = h2_pal_audio_track_drain(track, 3000u);
  }
  if (track != NULL) {
    const int close_rc = h2_pal_audio_track_close(track);
    if (rc == H2_AUDIO_OK) rc = close_rc;
  }
  const int stop_rc = h2_pal_audio_stop_speaker(audio);
  if (rc == H2_AUDIO_OK) rc = stop_rc;
  if (rc == H2_AUDIO_OK && (allocator_probe.live != 0u ||
      allocator_probe.allocations != allocator_probe.frees))
    rc = H2_AUDIO_ERR_INVALID_STATE;
  allocator_probe.passed = rc == H2_AUDIO_OK;
  return rc;
}

int h2_audio_mobile_run(const h2_pal_audio_api_t *audio,
                        const h2_pal_time_api_t *time,
                        h2_pal_audio_e2e_result_t *result) {
  if (result == NULL) return H2_AUDIO_ERR_INVALID_ARG;
  memset(result, 0, sizeof(*result));
  const int probe_rc = probe_track_allocator(audio);
  if (probe_rc != H2_AUDIO_OK) return probe_rc;
  const h2_pal_audio_e2e_config_t config = {
      .audio = audio, .time = time, .stability_ms = 30000u};
  return h2_pal_audio_e2e_run(&config, result);
}

int h2_audio_mobile_report(const char *path, const char *platform,
                           const char *version,
                           const h2_pal_audio_e2e_result_t *result,
                           int run_rc, int teardown_rc) {
  if (path == NULL || platform == NULL || version == NULL || result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  FILE *file = fopen(path, "w");
  if (file == NULL) return H2_PAL_ERR_WRITE;
  fprintf(file, "{\"platform\":\"%s\",\"image_version\":\"%s\","
                "\"passed\":%u,\"failed\":%u,\"blocked\":%u,"
                "\"mic_frames\":%u,\"mic_peak\":%u,\"mic_energy\":%llu,"
                "\"speaker_frames\":%u,\"output_peak\":%u,"
                "\"stability_elapsed_ms\":%llu,"
                "\"allocator_probe_passed\":%d,\"allocator_failure_rejected\":%d,"
                "\"allocator_allocations\":%u,\"allocator_frees\":%u,"
                "\"run_rc\":%d,\"teardown_rc\":%d,\"cases\":[",
          platform, version, result->passed, result->failed, result->blocked,
          (unsigned)result->mic_frames, (unsigned)result->mic_peak,
          (unsigned long long)result->mic_energy,
          (unsigned)result->speaker_frames, (unsigned)result->output_peak,
          (unsigned long long)result->stability_elapsed_ms,
          allocator_probe.passed, allocator_probe.failure_rejected,
          allocator_probe.allocations, allocator_probe.frees,
          run_rc, teardown_rc);
  for (size_t i = 0u; i < H2_PAL_AUDIO_E2E_CASE_COUNT; ++i) {
    const h2_pal_audio_e2e_case_result_t *item = &result->cases[i];
    fprintf(file, "%s{\"id\":\"%s\",\"status\":\"%s\","
                  "\"detail\":%d,\"line\":%u}",
            i == 0u ? "" : ",", item->id ? item->id : "missing",
            item->passed ? "PASS" : item->blocked ? "BLOCKED" : "FAIL",
            item->detail, item->line);
  }
  const int wrote = fputs("]}\n", file) >= 0;
  const int closed = fclose(file) == 0;
  if (!wrote || !closed) return H2_PAL_ERR_WRITE;
  return run_rc == H2_AUDIO_OK && teardown_rc == H2_PAL_OK &&
                 allocator_probe.passed && allocator_probe.failure_rejected &&
                 result->passed == H2_PAL_AUDIO_E2E_CASE_COUNT &&
                 result->failed == 0u && result->blocked == 0u &&
                 result->mic_frames >= 2u && result->speaker_frames >= 2u &&
                 result->stability_elapsed_ms >= 30000u
             ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
