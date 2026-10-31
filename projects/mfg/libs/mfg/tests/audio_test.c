#include "../src/h2_mfg_internal.h"
#include "h2_desktop_platform.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { RATE = 16000, SAMPLES = 512, QUEUE_MAX = 32 };

typedef struct fake_audio {
  h2_pal_audio_track_t track;
  int16_t queued[QUEUE_MAX][SAMPLES];
  size_t capacity;
  unsigned head, count, played, underruns, writes, reads;
  unsigned capture_pending;
  uint32_t volume;
  int speaker, mic, close_result, silent, restore_result;
} fake_audio_t;

static const h2_audio_pcm_format_t format = {
    .sample_rate_hz = RATE,
    .frame_samples_per_channel = SAMPLES,
    .channels = 1,
    .sample_format = H2_AUDIO_SAMPLE_S16LE};

static int info(void* user, h2_audio_info_t* out) {
  (void)user;
  *out = (h2_audio_info_t){.available = 1,
                           .mic_supported = 1,
                           .playback_supported = 1,
                           .mic_format = format,
                           .playback_format = format};
  return 0;
}

static int start_speaker(void* user) {
  ((fake_audio_t*)user)->speaker = 1;
  return 0;
}

static int stop_speaker(void* user) {
  fake_audio_t* a = user;
  /* The track borrows the mixer and must already be closed. */
  assert(a->capacity == 0);
  a->speaker = 0;
  return 0;
}

static int start_mic(void* user) {
  ((fake_audio_t*)user)->mic = 1;
  return 0;
}

static int stop_mic(void* user) {
  ((fake_audio_t*)user)->mic = 0;
  return 0;
}

static int get_volume(void* user, uint32_t* out) {
  *out = ((fake_audio_t*)user)->volume;
  return 0;
}

static int set_volume(void* user, uint32_t percent) {
  fake_audio_t* a = user;
  if (percent == 91 && a->restore_result != 0) return a->restore_result;
  a->volume = percent;
  return 0;
}

static int write_frame(h2_pal_audio_track_t* track,
                       const h2_audio_frame_t* frame, uint32_t timeout) {
  fake_audio_t* a = track->user;
  assert(a->speaker && timeout == 0);
  assert(frame->bytes == SAMPLES * sizeof(int16_t));
  if (a->count == a->capacity) return H2_PAL_ERR_WOULD_BLOCK;
  memcpy(a->queued[(a->head + a->count) % QUEUE_MAX], frame->data,
         frame->bytes);
  ++a->count;
  ++a->writes;
  return 0;
}

static int close_track(h2_pal_audio_track_t* track) {
  fake_audio_t* a = track->user;
  if (a->close_result != 0) return a->close_result;
  a->capacity = 0;
  return 0;
}

static int create_track(void* user, const h2_audio_track_config_t* config,
                        h2_pal_audio_track_t** out) {
  fake_audio_t* a = user;
  assert(config->buffer_frames <= QUEUE_MAX);
  a->capacity = config->buffer_frames;
  a->track = (h2_pal_audio_track_t){
      .user = a, .write = write_frame, .close = close_track};
  *out = &a->track;
  return 0;
}

static int read_mic(void* user, h2_audio_frame_t* frame, uint32_t timeout) {
  fake_audio_t* a = user;
  assert(a->mic && timeout == 0);
  if (a->capture_pending == 0) return H2_PAL_ERR_WOULD_BLOCK;
  --a->capture_pending;
  ++a->reads;
  int16_t* pcm = frame->data;
  for (unsigned i = 0; i < SAMPLES; ++i)
    pcm[i] = a->silent || a->reads % 3u == 0 ? 0 : (i % 2u == 0 ? 4096 : -4096);
  frame->bytes = SAMPLES * sizeof(*pcm);
  return 0;
}

static const h2_pal_audio_vtable_t vtable = {
    .get_info = info,
    .start_speaker = start_speaker,
    .stop_speaker = stop_speaker,
    .start_mic = start_mic,
    .stop_mic = stop_mic,
    .get_speaker_volume_percent = get_volume,
    .set_speaker_volume_percent = set_volume,
    .create_track = create_track,
    .mic_read = read_mic};

static void playback_until(fake_audio_t* a, uint64_t now) {
  unsigned due = (unsigned)(now * RATE / 1000u / SAMPLES);
  while (a->played < due) {
    if (a->count == 0) {
      ++a->underruns;
    } else {
      a->head = (a->head + 1u) % QUEUE_MAX;
      --a->count;
    }
    ++a->played;
  }
}

static void speaker_survives_slow_display(h2_mfg_t* m, fake_audio_t* a) {
  assert(h2_mfg_hardware_enter(m, 0) == 0);
  const char* action = NULL;
  for (uint64_t now = 0; now <= 4000; now += 125) {
    /* The real 466x466 panel makes a UI iteration take about 125 ms. The
     * consumer still needs a complete 512-sample frame every 32 ms. */
    playback_until(a, now);
    assert(h2_mfg_hardware_tick(m, now, &action) == 0 && action == NULL);
  }
  if (a->underruns != 0)
    fprintf(stderr, "speaker: %u missing frames after %u real-time frames\n",
            a->underruns, a->played);
  assert(a->underruns == 0 && a->played == 125);
  assert(m->audio_samples >= 4u * RATE);
  assert(m->model.ready && m->model.summary.step_status[0] == 0);
  assert(a->volume == 100);
  /* The queue was already full at the last tick. A rejected write must not
   * consume a frame or advance the tone's position. */
  uint64_t samples = m->audio_samples;
  assert(h2_mfg_hardware_tick(m, 4000, &action) == 0);
  assert(m->audio_samples == samples);
  a->close_result = H2_PAL_ERR_BUSY;
  void* pcm = m->pcm;
  assert(h2_mfg_hardware_leave(m) == H2_PAL_ERR_BUSY);
  assert(m->pcm == pcm && m->track != NULL && a->speaker);
  a->close_result = 0;
  a->restore_result = H2_PAL_ERR_IO;
  assert(h2_mfg_hardware_leave(m) == H2_PAL_ERR_IO);
  assert(m->volume_saved && m->pcm == pcm && a->volume == 100);
  assert(m->track == NULL && !a->speaker);
  a->restore_result = 0;
  assert(h2_mfg_hardware_leave(m) == 0);
  assert(m->pcm == NULL && m->track == NULL && !a->speaker && a->volume == 91);
}

static void microphone_drains_pending_capture(h2_mfg_t* m, fake_audio_t* a) {
  assert(h2_mfg_hardware_enter(m, 0) == 0);
  const char* action = NULL;
  for (unsigned tick = 0; tick < 14; ++tick) {
    a->capture_pending = 3;
    assert(h2_mfg_hardware_tick(m, tick * 125u, &action) == 0);
    assert(a->capture_pending == 0 && a->reads == (tick + 1u) * 3u);
    assert(action == NULL && m->model.summary.step_status[0] == 0);
    if (tick < 13) assert(!m->model.ready);
  }
  assert(m->audio_frames == 42 && m->mic_peak == 4096 && m->model.ready);
  assert(m->mic_level == 4096);
  a->capture_pending = 2;
  a->silent = 1;
  assert(h2_mfg_hardware_tick(m, 1800, &action) == 0);
  assert(m->mic_level == 0 && m->mic_peak == 4096);
  a->silent = 0;
  a->capture_pending = 2;
  assert(h2_mfg_hardware_tick(m, 1900, &action) == 0 && m->mic_level == 4096);
  assert(h2_mfg_hardware_tick(m, 2200, &action) == 0 && m->mic_level == 0);
  assert(h2_mfg_hardware_leave(m) == 0 && !a->mic && m->pcm == NULL);
}

int main(int argc, char** argv) {
  assert(argc <= 2);
  fake_audio_t a = {.volume = 91};
  const h2_pal_audio_api_t audio = {.user = &a, .vtable = &vtable};
  h2_runtime_t rt = {.mem = h2_desktop_platform_default_allocator(),
                     .audio = &audio};
  h2_mfg_step_t step = {"speaker", H2_MFG_SPEAKER, 0};
  const h2_mfg_profile_t profile = {
      .board = "test", .steps = &step, .total = 1, .speaker_test_volume = 100};
  h2_mfg_t m = {.rt = &rt,
                .profile = &profile,
                .model = {.profile = &profile, .summary = {.total = 1}}};
  if (argc == 1 || strcmp(argv[1], "speaker") == 0)
    speaker_survives_slow_display(&m, &a);
  if (argc == 1 || strcmp(argv[1], "mic") == 0) {
    step = (h2_mfg_step_t){"mic", H2_MFG_MIC, 0};
    microphone_drains_pending_capture(&m, &a);
  }
  return 0;
}
