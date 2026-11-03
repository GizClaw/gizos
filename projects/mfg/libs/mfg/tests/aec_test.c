#include "../src/h2_mfg_internal.h"
#include "h2_desktop_platform.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SAMPLES = 512, MAX_QUEUE = 8, WINDOW_SAMPLES = 96 * 512 };
typedef struct fake_recording {
  h2_mfg_recording_store_t api;
  h2_mfg_profile_t profile;
  /* The backend's storage is deliberately outside the PAL RAM budget. */
  int16_t* pcm;
  int16_t* expected;
  unsigned count, frames, samples, opens, closes;
  unsigned begins[2], writes[2], reads[2], accepted[2], played[2];
  int live, open_error, begin_error, write_error, read_error, close_error;
  int stopped_write_error;
} fake_recording_t;

typedef struct fake_window {
  unsigned samples, frames, dac, active, warmup;
  uint64_t elapsed;
} fake_window_t;

typedef struct fake {
  h2_mfg_t mfg;
  h2_runtime_t rt;
  h2_pal_audio_api_t audio;
  h2_pal_time_api_t time;
  h2_audio_aec_observer_t observer;
  h2_pal_audio_track_t track;
  uint64_t now, next, sequence;
  unsigned volume, head, count, capacity, pending, replay_frames;
  unsigned frame_samples, mic_head, mic_offset, echo_replay_frames;
  int speaker, mic, human_speech, no_echo_cancel, missing_reference, muted;
  int clipping, close_error, unregister_error, missing_dac;
  int opaque, disabled, register_error, partial_reads;
  int output_clipping, missing_capture, drain_error;
  int external, in_capture, block_replay, defer_final_recording;
  unsigned tick_ms, blocked_replay_writes;
  int in_service;
  unsigned capture_start_delay_ms, playback_start_delay_ms;
  uint64_t capture_not_before, playback_not_before;
  h2_pal_log_api_t log;
  fake_window_t windows[16];
  unsigned window_count;
  h2_pal_queue_api_t recording_queue_api;
  h2_pal_queue_t* capture_queue;
  fake_recording_t recording;
  int16_t queue[MAX_QUEUE][SAMPLES];
  int16_t captured[MAX_QUEUE][SAMPLES];
} fake_t;

static const h2_audio_pcm_format_t format = {
    .sample_rate_hz = 16000,
    .frame_samples_per_channel = SAMPLES,
    .channels = 1,
    .sample_format = H2_AUDIO_SAMPLE_S16LE};
static const h2_mfg_step_t step = {"aec", H2_MFG_AEC, 0};
static const h2_mfg_profile_t profile = {
    .board = "test", .revision = 1, .steps = &step, .total = 1};

static h2_audio_pcm_format_t pcm_format(const fake_t* f) {
  h2_audio_pcm_format_t out = format;
  out.frame_samples_per_channel = (uint16_t)f->frame_samples;
  return out;
}

static int info(void* user, h2_audio_info_t* out) {
  fake_t* f = user;
  *out = (h2_audio_info_t){.available = 1,
                           .mic_supported = 1,
                           .playback_supported = 1,
                           .aec_enabled = !f->disabled,
                           .mic_format = pcm_format(f),
                           .playback_format = pcm_format(f)};
  return 0;
}
static int get_volume(void* user, uint32_t* out) {
  *out = ((fake_t*)user)->volume;
  return 0;
}
static int set_volume(void* user, uint32_t value) {
  ((fake_t*)user)->volume = value;
  return 0;
}
static int gain(void* user, uint32_t* out) {
  (void)user;
  *out = 63;
  return 0;
}
static int observer(void* user, const h2_audio_aec_observer_t* o) {
  fake_t* f = user;
  assert(!f->in_service && !f->speaker && !f->mic);
  if (f->opaque) return H2_PAL_ERR_UNSUPPORTED;
  if (o != NULL && f->register_error) return H2_PAL_ERR_IO;
  if (o == NULL && f->unregister_error) return H2_PAL_ERR_BUSY;
  f->observer = o == NULL ? (h2_audio_aec_observer_t){0} : *o;
  return 0;
}
static int start_mic(void* user) {
  fake_t* f = user;
  assert(!f->in_service);
  f->capture_not_before =
      f->now + f->capture_start_delay_ms + f->frame_samples / 16;
  f->mic = 1;
  f->pending = 0;
  f->mic_head = f->mic_offset = 0;
  f->sequence = 0;
  f->next = f->now + f->frame_samples / 16;
  return 0;
}
static int stop_mic(void* user) {
  fake_t* f = user;
  assert(!f->in_service);
  f->mic = 0;
  return 0;
}
static int start_speaker(void* user) {
  fake_t* f = user;
  assert(!f->in_service);
  f->playback_not_before =
      f->now + f->playback_start_delay_ms + f->frame_samples / 16;
  f->speaker = 1;
  f->next = f->now + f->frame_samples / 16;
  return 0;
}
static int stop_speaker(void* user) {
  fake_t* f = user;
  assert(!f->in_service && f->capacity == 0);
  f->speaker = 0;
  return 0;
}
static int close_track(h2_pal_audio_track_t* t) {
  fake_t* f = t->user;
  assert(!f->in_service);
  if (f->close_error) return H2_PAL_ERR_BUSY;
  f->capacity = f->count = 0;
  return 0;
}
static int16_t* recorded_frame(const fake_t* f, int16_t* pcm,
                               unsigned recording, unsigned frame) {
  assert(pcm != NULL && recording < f->recording.count &&
         frame < f->recording.frames);
  return pcm + ((size_t)recording * f->recording.frames + frame) *
                   f->recording.samples;
}

static unsigned replay_recording(const fake_t* f) {
  return strcmp(h2_mfg_aec_phase(&f->mfg), "LISTEN FOR ECHO") == 0 || !f->opaque
             ? 0
             : 1;
}

static int write_frame(h2_pal_audio_track_t* t, const h2_audio_frame_t* frame,
                       uint32_t timeout) {
  fake_t* f = t->user;
  assert(timeout == 0 && f->speaker);
  assert(frame->bytes == f->frame_samples * sizeof(int16_t));
  if (f->external && !f->mic && f->block_replay) {
    ++f->blocked_replay_writes;
    return f->block_replay == 2 ? H2_PAL_ERR_TIMEOUT : H2_PAL_ERR_WOULD_BLOCK;
  }
  if (f->count == f->capacity) return H2_PAL_ERR_WOULD_BLOCK;
  if (f->external && !f->mic) {
    unsigned recording = replay_recording(f);
    unsigned position = f->recording.accepted[recording] % f->recording.frames;
    assert(f->recording.reads[recording] ==
           f->recording.accepted[recording] + 1);
    assert(memcmp(frame->data,
                  recorded_frame(f, f->recording.pcm, recording, position),
                  frame->bytes) == 0);
    ++f->recording.accepted[recording];
  }
  memcpy(f->queue[(f->head + f->count) % MAX_QUEUE], frame->data, frame->bytes);
  ++f->count;
  return 0;
}
static void advance(fake_t* f, uint64_t target);
static int drain(h2_pal_audio_track_t* t, uint32_t timeout) {
  fake_t* f = t->user;
  assert(!f->in_service && timeout == 1000);
  if (f->drain_error) return H2_PAL_ERR_IO;
  advance(f, f->now + f->count * f->frame_samples / 16);
  return 0;
}
static int create_track(void* user, const h2_audio_track_config_t* c,
                        h2_pal_audio_track_t** out) {
  fake_t* f = user;
  assert(!f->in_service && c->buffer_frames <= MAX_QUEUE);
  f->capacity = c->buffer_frames;
  f->head = f->count = 0;
  f->track = (h2_pal_audio_track_t){
      .user = f, .write = write_frame, .close = close_track, .drain = drain};
  *out = &f->track;
  return 0;
}
static int mic_read(void* user, h2_audio_frame_t* out, uint32_t timeout) {
  fake_t* f = user;
  assert(timeout == 0 && f->mic);
  if (f->pending == 0) return H2_PAL_ERR_WOULD_BLOCK;
  unsigned samples = f->partial_reads ? f->frame_samples / 2 : f->frame_samples;
  memcpy(out->data, f->captured[f->mic_head] + f->mic_offset,
         samples * sizeof(int16_t));
  out->bytes = samples * sizeof(int16_t);
  out->samples_per_channel = (uint16_t)samples;
  f->mic_offset += samples;
  if (f->mic_offset == f->frame_samples) {
    f->mic_offset = 0;
    f->mic_head = (f->mic_head + 1) % MAX_QUEUE;
    --f->pending;
  }
  return 0;
}
static h2_pal_result_t now_ms(void* user, uint64_t* out) {
  *out = ((fake_t*)user)->now;
  return 0;
}
static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms = now_ms};
static const h2_pal_audio_vtable_t audio_vtable = {
    .get_info = info,
    .get_speaker_volume_percent = get_volume,
    .set_speaker_volume_percent = set_volume,
    .get_mic_gain_percent = gain,
    .set_aec_observer = observer,
    .start_mic = start_mic,
    .stop_mic = stop_mic,
    .start_speaker = start_speaker,
    .stop_speaker = stop_speaker,
    .create_track = create_track,
    .mic_read = mic_read};

static void advance(fake_t* f, uint64_t target) {
  while (f->speaker && f->next <= target) {
    f->now = f->next;
    f->next += f->frame_samples / 16;
    int16_t pcm[SAMPLES] = {0}, raw[SAMPLES * 4], reference[SAMPLES],
            out[SAMPLES];
    int had_pcm = f->count != 0;
    if (had_pcm) {
      memcpy(pcm, f->queue[f->head], f->frame_samples * sizeof(int16_t));
      f->head = (f->head + 1) % MAX_QUEUE;
      --f->count;
    }
    if (f->observer.on_playback != NULL && !f->missing_dac &&
        f->now >= f->playback_not_before) {
      h2_audio_frame_t frame =
          h2_audio_frame_for_buffer(pcm, sizeof(pcm), pcm_format(f));
      frame.bytes = f->frame_samples * sizeof(int16_t);
      f->observer.on_playback(f->observer.user, &frame);
    } else if (!f->mic) {
      ++f->replay_frames;
      if (strcmp(h2_mfg_aec_phase(&f->mfg), "LISTEN FOR ECHO") == 0)
        ++f->echo_replay_frames;
      if (f->external && had_pcm) {
        unsigned recording = replay_recording(f);
        unsigned position =
            f->recording.played[recording] % f->recording.frames;
        assert(f->recording.played[recording] <
               f->recording.accepted[recording]);
        assert(memcmp(pcm,
                      recorded_frame(f, f->recording.expected, recording,
                                     position),
                      f->frame_samples * sizeof(int16_t)) == 0);
        ++f->recording.played[recording];
      }
    }
    if (f->mic && f->now >= f->capture_not_before) {
      const char* phase = h2_mfg_aec_phase(&f->mfg);
      int speech = f->human_speech && (strcmp(phase, "SPEAK NOW") == 0 ||
                                       strcmp(phase, "SPEAK OVER TONE") == 0);
      for (unsigned i = 0; i < f->frame_samples; ++i) {
        int amplitude = 1024;
        if (f->external) amplitude += (int)((f->sequence * 13 + i * 7) % 97);
        int16_t voice = speech ? (int16_t)(i % 2 ? -amplitude : amplitude) : 0;
        raw[4 * i] = reference[i] = f->missing_reference ? 0 : pcm[i];
        raw[4 * i + 1] = raw[4 * i + 3] =
            f->clipping ? 31000 : (int16_t)(8 + pcm[i] / 2 + voice);
        raw[4 * i + 2] = 0;
        out[i] =
            f->muted
                ? 0
                : (int16_t)(8 + pcm[i] / (f->no_echo_cancel ? 2 : 32) + voice);
        if (f->output_clipping) out[i] = 31000;
      }
      h2_audio_pcm_format_t raw_format = pcm_format(f);
      raw_format.channels = 4;
      h2_audio_frame_t r =
          h2_audio_frame_for_buffer(raw, sizeof(raw), raw_format);
      h2_audio_frame_t ref = h2_audio_frame_for_buffer(
          reference, sizeof(reference), pcm_format(f));
      h2_audio_frame_t output =
          h2_audio_frame_for_buffer(out, sizeof(out), pcm_format(f));
      r.bytes = 4 * f->frame_samples * sizeof(int16_t);
      ref.bytes = output.bytes = f->frame_samples * sizeof(int16_t);
      h2_audio_aec_frame_t frame = {.sequence = ++f->sequence,
                                    .raw = &r,
                                    .reference_input = &ref,
                                    .processed = &output,
                                    .microphone_lane = 1,
                                    .reference_lane = 0,
                                    .microphone_mask = 10};
      if (f->external && (strcmp(phase, "SPEAK OVER TONE") == 0 ||
                          (f->opaque && strcmp(phase, "ECHO TEST") == 0))) {
        unsigned warmup =
            (WINDOW_SAMPLES + f->frame_samples - 1) / f->frame_samples;
        if (f->sequence > warmup &&
            f->sequence <= warmup + f->recording.frames) {
          unsigned recording =
              strcmp(phase, "ECHO TEST") == 0 || !f->opaque ? 0 : 1;
          unsigned position = (unsigned)f->sequence - warmup - 1;
          memcpy(recorded_frame(f, f->recording.expected, recording, position),
                 out, f->frame_samples * sizeof(int16_t));
        }
      }
      if (f->observer.on_capture != NULL) {
        f->in_capture = 1;
        f->observer.on_capture(f->observer.user, &frame);
        f->in_capture = 0;
      }
      if (f->pending < MAX_QUEUE && !f->missing_capture) {
        memcpy(f->captured[(f->mic_head + f->pending) % MAX_QUEUE], out,
               f->frame_samples * sizeof(int16_t));
        ++f->pending;
      }
    }
  }
  f->now = target;
}

static int recording_open(void* user, unsigned count, unsigned frames,
                          unsigned samples) {
  fake_t* f = user;
  fake_recording_t* r = &f->recording;
  assert(!f->in_capture && !f->mic && !f->speaker && !r->live);
  assert(count == (f->opaque ? 2u : 1u) && samples == f->frame_samples);
  assert(frames == (WINDOW_SAMPLES + samples - 1) / samples);
  ++r->opens;
  r->count = count;
  r->frames = frames;
  r->samples = samples;
  size_t bytes = (size_t)count * frames * samples * sizeof(int16_t);
  r->pcm = malloc(bytes);
  r->expected = malloc(bytes);
  assert(r->pcm != NULL && r->expected != NULL);
  memset(r->pcm, 0, bytes);
  memset(r->expected, 0, bytes);
  r->live = 1;
  return r->open_error;
}

static int recording_begin(void* user, unsigned recording) {
  fake_t* f = user;
  fake_recording_t* r = &f->recording;
  assert(!f->in_capture && !f->mic && !f->speaker && r->live);
  assert(recording < r->count);
  ++r->begins[recording];
  if (r->begin_error) return r->begin_error;
  r->writes[recording] = r->reads[recording] = 0;
  r->accepted[recording] = r->played[recording] = 0;
  memset(recorded_frame(f, r->pcm, recording, 0), 0,
         (size_t)r->frames * r->samples * sizeof(int16_t));
  memset(recorded_frame(f, r->expected, recording, 0), 0,
         (size_t)r->frames * r->samples * sizeof(int16_t));
  return 0;
}

static int recording_write(void* user, unsigned recording, unsigned frame,
                           const int16_t* pcm, unsigned samples) {
  fake_t* f = user;
  fake_recording_t* r = &f->recording;
  assert(!f->in_capture && r->live && samples == r->samples);
  assert(recording < r->count && r->begins[recording] != 0 &&
         frame == r->writes[recording]);
  if (r->write_error) return r->write_error;
  if (r->stopped_write_error && !f->mic && !f->speaker) return H2_PAL_ERR_IO;
  assert(memcmp(pcm, recorded_frame(f, r->expected, recording, frame),
                samples * sizeof(int16_t)) == 0);
  memcpy(recorded_frame(f, r->pcm, recording, frame), pcm,
         samples * sizeof(int16_t));
  ++r->writes[recording];
  return 0;
}

static int recording_read(void* user, unsigned recording, unsigned frame,
                          int16_t* pcm, unsigned samples) {
  fake_t* f = user;
  fake_recording_t* r = &f->recording;
  assert(!f->in_capture && r->live && samples == r->samples);
  assert(recording < r->count && r->writes[recording] == r->frames &&
         frame == r->reads[recording] % r->frames);
  if (r->read_error) return r->read_error;
  memcpy(pcm, recorded_frame(f, r->pcm, recording, frame),
         samples * sizeof(int16_t));
  ++r->reads[recording];
  return 0;
}

static int recording_close(void* user) {
  fake_t* f = user;
  fake_recording_t* r = &f->recording;
  assert(!f->in_capture && !f->mic && !f->speaker);
  ++r->closes;
  if (r->close_error) return r->close_error;
  if (r->live) {
    size_t bytes = (size_t)r->count * r->frames * r->samples * sizeof(int16_t);
    memset(r->pcm, 0, bytes);
    memset(r->expected, 0, bytes);
    free(r->pcm);
    free(r->expected);
    r->pcm = r->expected = NULL;
    r->live = 0;
  }
  return 0;
}

static int capture_queue_create(void* user, const h2_pal_queue_config_t* config,
                                h2_pal_queue_t** out) {
  fake_t* f = user;
  assert(!f->in_capture && !f->mic && !f->speaker && f->capture_queue == NULL);
  assert(config->item_count == 4 &&
         config->item_size == f->frame_samples * sizeof(int16_t) &&
         config->allocator == f->rt.mem);
  int rc = h2_pal_queue_create(h2_desktop_platform_queue_api(), config, out);
  if (rc == 0) f->capture_queue = *out;
  return rc;
}

static void capture_queue_destroy(void* user, h2_pal_queue_t* queue) {
  fake_t* f = user;
  assert(!f->in_capture && !f->mic && !f->speaker && queue == f->capture_queue);
  h2_pal_queue_destroy(h2_desktop_platform_queue_api(), queue);
  f->capture_queue = NULL;
}

static int capture_queue_send(void* user, h2_pal_queue_t* queue,
                              const void* item, uint32_t timeout) {
  fake_t* f = user;
  assert(f->in_capture && timeout == H2_PAL_QUEUE_NO_WAIT);
  return h2_pal_queue_send(h2_desktop_platform_queue_api(), queue, item,
                           timeout);
}

static int capture_queue_recv(void* user, h2_pal_queue_t* queue, void* item,
                              uint32_t timeout) {
  fake_t* f = user;
  assert(!f->in_capture && timeout == H2_PAL_QUEUE_NO_WAIT);
  if (f->defer_final_recording && f->mic &&
      f->sequence >= 2 * f->recording.frames)
    return H2_PAL_ERR_WOULD_BLOCK;
  return h2_pal_queue_recv(h2_desktop_platform_queue_api(), queue, item,
                           timeout);
}

static int capture_queue_reset(void* user, h2_pal_queue_t* queue) {
  fake_t* f = user;
  assert(!f->in_capture && !f->mic && !f->speaker);
  return h2_pal_queue_reset(h2_desktop_platform_queue_api(), queue);
}

static const h2_pal_queue_vtable_t recording_queue_vtable = {
    .create = capture_queue_create,
    .destroy = capture_queue_destroy,
    .send = capture_queue_send,
    .recv = capture_queue_recv,
    .reset = capture_queue_reset};

static int window_log(void* user, h2_pal_log_level_t level, const char* scope,
                      const char* message) {
  fake_t* f = user;
  const char* prefix = "H2_MFG_AEC_WINDOW phase=";
  assert(level == H2_PAL_LOG_INFO && strcmp(scope, "mfg/aec") == 0);
  if (strncmp(message, prefix, strlen(prefix)) != 0) return 0;
  assert(f->window_count < sizeof(f->windows) / sizeof(f->windows[0]));
  fake_window_t* window = &f->windows[f->window_count++];
  const char* fields = strstr(message, " samples=");
  uint64_t queued = 0;
  assert(fields != NULL);
  assert(sscanf(fields,
                " samples=%u frames=%u dac=%u active=%u queued=%" SCNu64
                " elapsed_ms=%" SCNu64 " warmup=%u",
                &window->samples, &window->frames, &window->dac,
                &window->active, &queued, &window->elapsed,
                &window->warmup) == 7);
  return 0;
}

static const h2_pal_log_vtable_t window_log_vtable = {.write = window_log};

static void init_format(fake_t* f, unsigned samples, int opaque) {
  memset(f, 0, sizeof(*f));
  f->frame_samples = samples;
  f->opaque = opaque;
  f->tick_ms = opaque ? 20 : 125;
  f->volume = 91;
  f->audio = (h2_pal_audio_api_t){.user = f, .vtable = &audio_vtable};
  f->time = (h2_pal_time_api_t){.user = f, .vtable = &time_vtable};
  f->rt = (h2_runtime_t){.mem = h2_desktop_platform_default_allocator(),
                         .audio = &f->audio,
                         .time = &f->time,
                         .queue = h2_desktop_platform_queue_api()};
  f->mfg = (h2_mfg_t){.rt = &f->rt,
                      .profile = &profile,
                      .model = {.profile = &profile, .summary = {.total = 1}}};
}

static void setup_format(fake_t* f, unsigned samples, int opaque) {
  init_format(f, samples, opaque);
  assert(h2_mfg_hardware_enter(&f->mfg, 0) == 0);
}

static void init_external(fake_t* f, unsigned samples, int opaque) {
  init_format(f, samples, opaque);
  f->external = 1;
  f->tick_ms = 20;
  f->recording.api = (h2_mfg_recording_store_t){.user = f,
                                                .open = recording_open,
                                                .begin = recording_begin,
                                                .write_frame = recording_write,
                                                .read_frame = recording_read,
                                                .close = recording_close};
  f->recording.profile = profile;
  f->recording.profile.aec_recording_store = &f->recording.api;
  f->mfg.profile = f->mfg.model.profile = &f->recording.profile;
  f->recording_queue_api =
      (h2_pal_queue_api_t){.user = f, .vtable = &recording_queue_vtable};
  f->rt.queue = &f->recording_queue_api;
}

static void setup_external(fake_t* f, unsigned samples, int opaque) {
  init_external(f, samples, opaque);
  assert(h2_mfg_hardware_enter(&f->mfg, 0) == 0);
}

static void setup(fake_t* f) { setup_format(f, SAMPLES, 0); }

static int tick(fake_t* f) {
  advance(f, f->now + f->tick_ms);
  const char* action = NULL;
  int rc = h2_mfg_hardware_tick(&f->mfg, f->now, &action);
  assert(action == NULL && f->mfg.model.summary.step_status[0] == 0);
  return rc;
}
static void teardown(fake_t* f) {
  assert(h2_mfg_hardware_leave(&f->mfg) == 0);
  assert(!f->mic && !f->speaker && f->observer.user == NULL &&
         f->mfg.aec == NULL && f->volume == 91);
}

static void operator_speech_and_replay_required(void) {
  fake_t f;
  setup(&f);
  while (f.now < 25000) assert(tick(&f) == 0 && !f.mfg.model.ready);
  assert(strcmp(h2_mfg_aec_phase(&f.mfg), "SPEAK NOW") == 0);
  assert(h2_mfg_aec_echo_db(&f.mfg) >= 10 && f.replay_frames == 0);
  f.human_speech = 1;
  while (!f.mfg.model.ready && f.now < 60000) assert(tick(&f) == 0);
  assert(f.mfg.model.ready && f.replay_frames >= 96);
  assert(strcmp(h2_mfg_aec_phase(&f.mfg), "LISTEN TO VOICE") == 0);
  teardown(&f);
}

static int draw_failure(void* user, const h2_display_rect_t* area,
                        const void* pixels, size_t stride,
                        h2_display_pixel_format_t format) {
  (void)user;
  assert(area->width == 480 && area->height == 480 && pixels != NULL);
  assert(stride == 960 && format == H2_DISPLAY_PIXEL_RGB565);
  return 0;
}
static int present_failure(void* user) {
  (void)user;
  return 0;
}
static void generated_echo_failure_shows_reason(fake_t* f) {
  const char* reason = strstr(f->mfg.detail, "echo reduction below 10dB");
  assert(reason != NULL && reason - f->mfg.detail > 50);
  char expected[64];
  strcpy(expected, reason);
  const h2_mfg_ui_config_t ui = {.width = 480,
                                 .height = 480,
                                 .failed = 0xf800,
                                 .text = 0xffff,
                                 .rows = {{48, 144, 384, 30},
                                          {48, 185, 384, 30},
                                          {48, 225, 384, 30},
                                          {48, 265, 384, 30}}};
  h2_mfg_profile_t configured = profile;
  configured.ui = &ui;
  const h2_pal_display_vtable_t vt = {.draw_bitmap = draw_failure,
                                      .present = present_failure};
  const h2_pal_display_api_t display = {.vtable = &vt};
  f->rt.display = &display;
  f->mfg.profile = &configured;
  f->mfg.model.error = H2_PAL_ERR_IO;
  f->mfg.display_open = 1;
  f->mfg.display.width = f->mfg.display.height = 480;
  size_t bytes = 480u * 480u * sizeof(uint16_t);
  f->mfg.frame = h2_pal_mem_alloc(f->rt.mem, bytes);
  uint16_t* saved = h2_pal_mem_alloc(f->rt.mem, bytes);
  assert(f->mfg.frame && saved);
  assert(h2_mfg_ui_draw(&f->mfg, 0) == 0);
  memcpy(saved, f->mfg.frame, bytes);
  strcpy(f->mfg.detail, expected);
  assert(h2_mfg_ui_draw(&f->mfg, 0) == 0);
  assert(memcmp(saved, f->mfg.frame, bytes) == 0);
  h2_pal_mem_free(f->rt.mem, saved);
  h2_pal_mem_free(f->rt.mem, f->mfg.frame);
  f->mfg.frame = NULL;
  f->mfg.display_open = 0;
  f->mfg.profile = &profile;
  f->rt.display = NULL;
}

static void reject_missing_reference_echo_clipping_and_dac(void) {
  for (unsigned which = 0; which < 4; ++which) {
    fake_t f;
    setup(&f);
    f.no_echo_cancel = which == 0;
    f.missing_reference = which == 1;
    f.clipping = which == 2;
    f.missing_dac = which == 3;
    int rc = 0;
    while (rc == 0 && f.now < 25000) rc = tick(&f);
    assert(rc != 0 && !f.mfg.model.ready);
    if (which == 0) generated_echo_failure_shows_reason(&f);
    teardown(&f);
  }
}

static void muted_path_and_failed_cleanup_do_not_pass(void) {
  fake_t f;
  setup(&f);
  f.muted = f.human_speech = 1;
  while (f.now < 30000) assert(tick(&f) == 0 && !f.mfg.model.ready);
  assert(strcmp(h2_mfg_aec_phase(&f.mfg), "SPEAK NOW") == 0);
  f.close_error = 1;
  assert(h2_mfg_hardware_leave(&f.mfg) == H2_PAL_ERR_BUSY);
  assert(f.mfg.aec != NULL && f.mfg.pcm != NULL && f.mfg.track != NULL);
  f.close_error = 0;
  f.unregister_error = 1;
  assert(h2_mfg_hardware_leave(&f.mfg) == H2_PAL_ERR_BUSY);
  assert(f.mfg.aec != NULL && f.observer.user != NULL);
  f.unregister_error = 0;
  teardown(&f);
}

static void board_frame_sizes_and_operator_acceptance(void) {
  const unsigned sizes[] = {256, 320, 512};
  for (unsigned opaque = 0; opaque < 2; ++opaque) {
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
      fake_t f;
      setup_format(&f, sizes[i], (int)opaque);
      f.partial_reads = opaque != 0;
      while (f.now < 25000) assert(tick(&f) == 0 && !f.mfg.model.ready);
      assert(strcmp(h2_mfg_aec_phase(&f.mfg), "SPEAK NOW") == 0);
      assert(opaque ? h2_mfg_aec_echo_db(&f.mfg) == -1
                    : h2_mfg_aec_echo_db(&f.mfg) >= 10);
      f.human_speech = 1;
      while (!f.mfg.model.ready && f.now < 65000) assert(tick(&f) == 0);
      assert(f.mfg.model.ready);
      if (opaque) {
        unsigned measured = (96 * 512 + sizes[i] - 1) / sizes[i];
        assert(f.echo_replay_frames >= measured);
        assert(f.replay_frames >= 2 * measured);
      }
      teardown(&f);
    }
  }
}

static void disabled_aec_and_observer_errors_cannot_fall_back(void) {
  fake_t f;
  setup(&f);
  teardown(&f);
  f.disabled = 1;
  f.opaque = 1;
  assert(h2_mfg_hardware_enter(&f.mfg, f.now) == H2_PAL_ERR_UNSUPPORTED);
  assert(f.mfg.aec == NULL && !f.mfg.model.ready);
  f.disabled = f.opaque = 0;
  f.register_error = 1;
  assert(h2_mfg_hardware_enter(&f.mfg, f.now) == H2_PAL_ERR_IO);
  assert(!f.mfg.model.ready && f.observer.user == NULL);
  teardown(&f);
}

static void opaque_mute_clipping_and_transport_failures(void) {
  fake_t f;
  setup_format(&f, 320, 1);
  f.muted = f.human_speech = 1;
  while (f.now < 30000) assert(tick(&f) == 0 && !f.mfg.model.ready);
  assert(strcmp(h2_mfg_aec_phase(&f.mfg), "SPEAK NOW") == 0);
  assert(f.echo_replay_frames == 0);
  f.close_error = 1;
  assert(h2_mfg_hardware_leave(&f.mfg) == H2_PAL_ERR_BUSY);
  assert(f.mfg.aec != NULL && f.mfg.pcm != NULL && f.mfg.track != NULL);
  f.close_error = 0;
  teardown(&f);

  for (unsigned which = 0; which < 3; ++which) {
    setup_format(&f, 320, 1);
    f.output_clipping = which == 0;
    f.missing_capture = which == 1;
    f.drain_error = which == 2;
    int rc = 0;
    while (rc == 0 && f.now < 25000) rc = tick(&f);
    assert(rc != 0 && !f.mfg.model.ready && f.echo_replay_frames == 0);
    teardown(&f);
  }
}

typedef struct memory_budget {
  size_t limit, used, peak;
  const int* in_capture;
} memory_budget_t;

typedef union budget_header {
  max_align_t alignment;
  size_t bytes;
} budget_header_t;

static void* budget_alloc(void* user, size_t bytes) {
  memory_budget_t* budget = user;
  assert(budget->in_capture == NULL || !*budget->in_capture);
  if (bytes > budget->limit - budget->used) return NULL;
  budget_header_t* allocation = malloc(sizeof(*allocation) + bytes);
  if (allocation == NULL) return NULL;
  allocation->bytes = bytes;
  budget->used += bytes;
  if (budget->used > budget->peak) budget->peak = budget->used;
  return allocation + 1;
}

static void budget_free(void* user, void* pointer) {
  memory_budget_t* budget = user;
  budget_header_t* allocation = (budget_header_t*)pointer - 1;
  assert(budget->used >= allocation->bytes);
  budget->used -= allocation->bytes;
  free(allocation);
}

static void constrained_memory_keeps_voice_and_cleanup(void) {
  static const h2_pal_mem_vtable_t vtable = {.alloc = budget_alloc,
                                             .free = budget_free};
  for (int opaque = 0; opaque < 2; ++opaque) {
    fake_t f;
    setup_format(&f, SAMPLES, opaque);
    teardown(&f);
    memory_budget_t budget = {.limit = 104u * 1024u};
    h2_pal_mem_api_t mem = {.user = &budget, .vtable = &vtable};
    f.rt.mem = &mem;
    int rc = h2_mfg_hardware_enter(&f.mfg, f.now);
    if (opaque) {
      /* Two recordings still need their real budget; no silent truncation. */
      assert(rc == H2_PAL_ERR_NO_MEMORY && !f.mfg.model.ready);
    } else {
      assert(rc == 0);
      f.human_speech = 1;
      while (!f.mfg.model.ready && f.now < 65000) assert(tick(&f) == 0);
      assert(f.mfg.model.ready && f.replay_frames >= 96);
      assert(h2_mfg_aec_echo_db(&f.mfg) >= 10);
    }
    teardown(&f);
    assert(budget.peak <= budget.limit && budget.used == 0);
    budget.limit = 0;
    assert(h2_mfg_hardware_enter(&f.mfg, f.now) == H2_PAL_ERR_NO_MEMORY);
    assert(f.mfg.aec == NULL && !f.mfg.model.ready && budget.used == 0);
    teardown(&f);
  }
}

static void wait_for_phase(fake_t* f, const char* phase) {
  uint64_t deadline = f->now + 65000;
  while (strcmp(h2_mfg_aec_phase(&f->mfg), phase) != 0 && f->now < deadline)
    assert(tick(f) == 0 && !f->mfg.model.ready);
  assert(strcmp(h2_mfg_aec_phase(&f->mfg), phase) == 0);
}

static void wait_for_replay(fake_t* f) {
  uint64_t deadline = f->now + 65000;
  while (!f->mfg.model.ready && f->now < deadline) assert(tick(f) == 0);
  assert(f->mfg.model.ready);
}

static void external_recordings_keep_all_pcm_with_small_memory(void) {
  static const h2_pal_mem_vtable_t vtable = {.alloc = budget_alloc,
                                             .free = budget_free};
  const unsigned sizes[] = {256, 320, 512};
  for (int opaque = 0; opaque < 2; ++opaque) {
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
      fake_t f;
      init_external(&f, sizes[i], opaque);
      memory_budget_t budget = {.limit = 12u * 1024u,
                                .in_capture = &f.in_capture};
      h2_pal_mem_api_t mem = {.user = &budget, .vtable = &vtable};
      f.rt.mem = &mem;
      f.human_speech = 1;
      f.partial_reads = opaque;
      /* Hold the final queued frames until both workers have joined. */
      f.defer_final_recording = !opaque;
      assert(h2_mfg_hardware_enter(&f.mfg, 0) == 0);
      assert(f.recording.opens == 1 && f.recording.count == (opaque ? 2u : 1u));
      assert(opaque ? f.capture_queue == NULL : f.capture_queue != NULL);
      wait_for_replay(&f);
      for (unsigned recording = 0; recording < f.recording.count; ++recording) {
        assert(f.recording.begins[recording] == 1);
        assert(f.recording.writes[recording] == f.recording.frames);
        assert(f.recording.reads[recording] == f.recording.frames);
        assert(f.recording.accepted[recording] == f.recording.frames);
        assert(f.recording.played[recording] == f.recording.frames);
        uint64_t samples = (uint64_t)f.recording.played[recording] * sizes[i];
        assert(samples >= WINDOW_SAMPLES &&
               samples < WINDOW_SAMPLES + sizes[i]);
      }
      assert(opaque ? h2_mfg_aec_echo_db(&f.mfg) == -1
                    : h2_mfg_aec_echo_db(&f.mfg) >= 10);
      teardown(&f);
      assert(f.recording.closes == 1 && !f.recording.live &&
             f.recording.pcm == NULL && f.capture_queue == NULL);
      assert(budget.peak <= budget.limit && budget.used == 0);
    }
  }
}

static void external_playback_backpressure_keeps_one_frame_loaded(void) {
  for (int opaque = 0; opaque < 2; ++opaque) {
    fake_t f;
    setup_external(&f, SAMPLES, opaque);
    f.human_speech = f.block_replay = 1;
    wait_for_phase(&f, opaque ? "LISTEN FOR ECHO" : "LISTEN TO VOICE");
    for (unsigned i = 0; i < 32; ++i) {
      f.block_replay = i < 16 ? 1 : 2;
      assert(tick(&f) == 0 && !f.mfg.model.ready);
      assert(f.recording.reads[0] == 1 && f.recording.accepted[0] == 0 &&
             f.recording.played[0] == 0);
    }
    assert(f.blocked_replay_writes == 32);
    f.block_replay = 0;
    wait_for_replay(&f);
    for (unsigned recording = 0; recording < f.recording.count; ++recording)
      assert(f.recording.reads[recording] == f.recording.frames &&
             f.recording.accepted[recording] == f.recording.frames &&
             f.recording.played[recording] == f.recording.frames);
    teardown(&f);
  }
}

static void external_retry_begins_a_fresh_voice_recording(void) {
  for (int opaque = 0; opaque < 2; ++opaque) {
    fake_t f;
    setup_external(&f, 320, opaque);
    f.partial_reads = opaque;
    f.human_speech = 1;
    wait_for_phase(&f, "SPEAK OVER TONE");
    f.human_speech = 0;
    unsigned voice = opaque ? 1 : 0;
    uint64_t deadline = f.now + 15000;
    while (f.recording.begins[voice] < 2 && f.now < deadline)
      assert(tick(&f) == 0 && !f.mfg.model.ready);
    assert(f.recording.begins[voice] == 2 && f.recording.writes[voice] == 0);
    f.human_speech = 1;
    wait_for_replay(&f);
    assert(f.recording.begins[voice] == 2 &&
           f.recording.writes[voice] == f.recording.frames &&
           f.recording.played[voice] == f.recording.frames);
    if (opaque)
      assert(f.recording.begins[0] == 1 &&
             f.recording.played[0] == f.recording.frames);
    teardown(&f);
  }
}

static void external_begin_write_and_read_errors_cannot_pass(void) {
  for (int opaque = 0; opaque < 2; ++opaque) {
    for (unsigned which = 0; which < 3; ++which) {
      fake_t f;
      setup_external(&f, 320, opaque);
      f.partial_reads = opaque;
      f.human_speech = 1;
      f.recording.begin_error = which == 0 ? H2_PAL_ERR_IO : 0;
      f.recording.write_error = which == 1 ? H2_PAL_ERR_IO : 0;
      f.recording.read_error = which == 2 ? H2_PAL_ERR_IO : 0;
      int rc = 0;
      while (rc == 0 && f.now < 65000) rc = tick(&f);
      assert(rc == H2_PAL_ERR_IO && !f.mfg.model.ready && f.mfg.aec != NULL &&
             f.mfg.pcm != NULL && f.recording.live);
      assert(f.recording.accepted[0] == 0 && f.recording.accepted[1] == 0);
      if (which == 0) assert(!f.mic && !f.speaker);
      if (which == 2)
        for (unsigned recording = 0; recording < f.recording.count; ++recording)
          assert(f.recording.writes[recording] == f.recording.frames);
      teardown(&f);
      assert(f.recording.closes == 1);
    }
  }
}

static void external_queue_overflow_and_final_drain_errors_cannot_pass(void) {
  for (unsigned which = 0; which < 2; ++which) {
    fake_t f;
    setup_external(&f, SAMPLES, 0);
    f.human_speech = 1;
    wait_for_phase(&f, "SPEAK OVER TONE");
    if (which == 0) {
      /* More than four native frames arrive between owner drains. */
      f.tick_ms = 250;
    } else {
      f.defer_final_recording = f.recording.stopped_write_error = 1;
    }
    int rc = 0;
    uint64_t deadline = f.now + 15000;
    while (rc == 0 && f.now < deadline) rc = tick(&f);
    assert(rc == H2_PAL_ERR_IO);
    assert(!f.mfg.model.ready && f.recording.reads[0] == 0 &&
           f.recording.writes[0] < f.recording.frames);
    if (which != 0) assert(!f.mic && !f.speaker);
    teardown(&f);
  }
}

static void external_close_retains_dependencies_after_partial_open(void) {
  for (int opaque = 0; opaque < 2; ++opaque) {
    for (unsigned failed_open = 0; failed_open < 2; ++failed_open) {
      fake_t f;
      init_external(&f, SAMPLES, opaque);
      f.recording.open_error = failed_open ? H2_PAL_ERR_IO : 0;
      assert(h2_mfg_hardware_enter(&f.mfg, 0) == f.recording.open_error);
      assert(f.recording.opens == 1 && f.recording.live);
      h2_pal_queue_t* queue = f.capture_queue;
      int16_t* pcm = f.recording.pcm;
      f.recording.close_error = H2_PAL_ERR_BUSY;
      assert(h2_mfg_hardware_leave(&f.mfg) == H2_PAL_ERR_BUSY);
      assert(f.recording.closes == 1 && f.recording.live &&
             f.recording.pcm == pcm && f.capture_queue == queue &&
             f.mfg.aec != NULL && f.mfg.pcm != NULL && f.mfg.volume_saved &&
             !f.mic && !f.speaker && f.observer.user == NULL);
      f.recording.close_error = 0;
      teardown(&f);
      assert(f.recording.closes == 2 && !f.recording.live &&
             f.recording.pcm == NULL && f.capture_queue == NULL);
      assert(h2_mfg_hardware_leave(&f.mfg) == 0 && f.recording.closes == 2);
    }
  }
}

static void external_allocation_failure_keeps_cleanup_available(void) {
  static const h2_pal_mem_vtable_t vtable = {.alloc = budget_alloc,
                                             .free = budget_free};
  fake_t f;
  init_external(&f, SAMPLES, 0);
  memory_budget_t budget = {.limit = 4u * 1024u, .in_capture = &f.in_capture};
  h2_pal_mem_api_t mem = {.user = &budget, .vtable = &vtable};
  f.rt.mem = &mem;
  assert(h2_mfg_hardware_enter(&f.mfg, 0) == H2_PAL_ERR_NO_MEMORY);
  assert(f.mfg.aec != NULL && f.mfg.pcm != NULL && f.recording.opens == 0 &&
         f.capture_queue == NULL && !f.mfg.model.ready);
  teardown(&f);
  assert(budget.used == 0 && f.recording.closes == 0);
}

static void external_paths_preserve_acoustic_and_transport_rejection(void) {
  for (unsigned which = 0; which < 7; ++which) {
    fake_t f;
    setup_external(&f, 320, which >= 4);
    f.no_echo_cancel = which == 0;
    f.missing_reference = which == 1;
    f.clipping = which == 2;
    f.missing_dac = which == 3;
    f.output_clipping = which == 4;
    f.missing_capture = which == 5;
    f.drain_error = which == 6;
    int rc = 0;
    while (rc == 0 && f.now < 25000) rc = tick(&f);
    assert(rc != 0 && !f.mfg.model.ready && f.recording.reads[0] == 0);
    teardown(&f);
  }
  for (int opaque = 0; opaque < 2; ++opaque) {
    fake_t f;
    setup_external(&f, 320, opaque);
    f.muted = f.human_speech = 1;
    while (f.now < 30000) assert(tick(&f) == 0 && !f.mfg.model.ready);
    assert(strcmp(h2_mfg_aec_phase(&f.mfg), "SPEAK NOW") == 0 &&
           f.recording.begins[opaque ? 1 : 0] == 0);
    teardown(&f);
  }
}

static void init_slow_ui(fake_t* f, unsigned samples, int opaque,
                         int external) {
  if (external) {
    init_external(f, samples, opaque);
  } else {
    init_format(f, samples, opaque);
    f->recording.profile = profile;
    f->mfg.profile = f->mfg.model.profile = &f->recording.profile;
  }
  f->recording.profile.playback_buffer_frames = 2;
  f->human_speech = 1;
  f->partial_reads = opaque;
  f->log = (h2_pal_log_api_t){.user = f, .vtable = &window_log_vtable};
  f->rt.log = &f->log;
}

static int controller_tick(fake_t* f) {
  const char* action = NULL;
  int rc = h2_mfg_hardware_tick(&f->mfg, f->now, &action);
  assert(action == NULL && f->mfg.model.summary.step_status[0] == 0);
  return rc;
}

static int service(fake_t* f) {
  const char* phase = h2_mfg_aec_phase(&f->mfg);
  h2_pal_audio_track_t* track = f->mfg.track;
  unsigned windows = f->window_count;
  int ready = f->mfg.model.ready, mic = f->mic, speaker = f->speaker;
  f->in_service = 1;
  int rc = h2_mfg_aec_service(&f->mfg, f->now);
  f->in_service = 0;
  assert(strcmp(h2_mfg_aec_phase(&f->mfg), phase) == 0 &&
         f->mfg.track == track && f->mic == mic && f->speaker == speaker &&
         f->window_count == windows);
  assert(rc == 0 ? f->mfg.model.ready == ready : !f->mfg.model.ready);
  return rc;
}

static int slow_ui_tick(fake_t* f) {
  /* A complete strip-rendered redraw takes 192 ms. Cooperative owner service
   * runs between its 24 eight-ms strips, without completing any live window. */
  for (unsigned strip = 0; strip < 24; ++strip) {
    advance(f, f->now + 8);
    int rc = service(f);
    if (rc != 0) return rc;
  }
  return controller_tick(f);
}

static void slow_ui_until_phase(fake_t* f, const char* phase) {
  uint64_t deadline = f->now + 65000;
  while (strcmp(h2_mfg_aec_phase(&f->mfg), phase) != 0 && f->now < deadline)
    assert(slow_ui_tick(f) == 0 && !f->mfg.model.ready);
  assert(strcmp(h2_mfg_aec_phase(&f->mfg), phase) == 0);
}

static void slow_ui_with_two_playback_frames_keeps_complete_windows(void) {
  const unsigned sizes[] = {256, 320, 512};
  for (int external = 0; external < 2; ++external) {
    for (int opaque = 0; opaque < 2; ++opaque) {
      for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        fake_t f;
        init_slow_ui(&f, sizes[i], opaque, external);
        assert(h2_mfg_hardware_enter(&f.mfg, 0) == 0 && f.capacity == 2);
        while (!f.mfg.model.ready && f.now < 65000)
          assert(slow_ui_tick(&f) == 0);
        assert(f.mfg.model.ready && f.window_count == 5);
        unsigned measured = (WINDOW_SAMPLES + sizes[i] - 1) / sizes[i];
        for (unsigned window = 0; window < f.window_count; ++window) {
          const fake_window_t* w = &f.windows[window];
          int far = window == 1 || window == 4;
          assert(w->samples == sizes[i] && w->frames == measured);
          assert(w->dac == (opaque ? 0 : measured));
          assert(w->active == (opaque || !far ? 0 : measured));
          uint64_t nominal = (uint64_t)(w->warmup + measured) * sizes[i] / 16;
          assert(w->elapsed + 1 >= nominal &&
                 w->elapsed <= nominal * 5 / 4 + 100);
        }
        if (external)
          for (unsigned recording = 0; recording < f.recording.count;
               ++recording)
            assert(f.recording.writes[recording] == measured &&
                   f.recording.reads[recording] == measured &&
                   f.recording.accepted[recording] == measured &&
                   f.recording.played[recording] == measured);
        teardown(&f);
      }
    }
  }
}

static void unserviced_two_frame_playback_still_rejects_starvation(void) {
  fake_t f;
  init_slow_ui(&f, SAMPLES, 0, 1);
  assert(h2_mfg_hardware_enter(&f.mfg, 0) == 0);
  f.tick_ms = 192;
  int rc = 0;
  while (rc == 0 && f.now < 25000) rc = tick(&f);
  assert(rc == H2_PAL_ERR_INVALID_STATE && !f.mfg.model.ready &&
         f.window_count == 2 && f.windows[1].frames == 96 &&
         f.windows[1].dac == 96 && f.windows[1].active < 96);
  teardown(&f);
}

static void complete_capture_and_dac_are_required_before_finishing(void) {
  for (unsigned delayed_worker = 0; delayed_worker < 2; ++delayed_worker) {
    fake_t f;
    init_slow_ui(&f, SAMPLES, 0, 1);
    f.capture_start_delay_ms = delayed_worker == 0 ? 640 : 0;
    f.playback_start_delay_ms = delayed_worker == 1 ? 640 : 0;
    assert(h2_mfg_hardware_enter(&f.mfg, 0) == 0);
    const uint64_t nominal = (32u + 96u) * 32u;
    while (f.now < nominal + 500) assert(slow_ui_tick(&f) == 0);
    assert(strcmp(h2_mfg_aec_phase(&f.mfg), "QUIET") == 0 &&
           f.window_count == 0 && !f.mfg.model.ready && f.mic && f.speaker);
    while (f.window_count == 0 && f.now < nominal * 5 / 4 + 100)
      assert(slow_ui_tick(&f) == 0);
    assert(f.window_count == 1 && f.windows[0].frames == 96 &&
           f.windows[0].dac == 96 && f.windows[0].active == 0 &&
           f.windows[0].elapsed == nominal + 640 &&
           strcmp(h2_mfg_aec_phase(&f.mfg), "ECHO TEST") == 0);
    teardown(&f);
  }
  fake_t f;
  init_slow_ui(&f, SAMPLES, 0, 1);
  f.capture_start_delay_ms = 2000;
  assert(h2_mfg_hardware_enter(&f.mfg, 0) == 0);
  int rc = 0;
  while (rc == 0 && f.now < 10000) rc = slow_ui_tick(&f);
  uint64_t timeout = (32u + 96u) * 32u * 5 / 4 + 100;
  assert(rc == H2_PAL_ERR_INVALID_STATE && !f.mfg.model.ready &&
         f.window_count == 1 && f.windows[0].frames < 96 &&
         f.windows[0].dac == 96 && f.now >= timeout && f.now < timeout + 192 &&
         !f.mic && !f.speaker);
  teardown(&f);
}

static void service_exhaustion_and_repeat_wait_for_controller_tick(void) {
  for (int opaque = 0; opaque < 2; ++opaque) {
    fake_t f;
    init_slow_ui(&f, SAMPLES, opaque, 1);
    assert(h2_mfg_hardware_enter(&f.mfg, 0) == 0);
    slow_ui_until_phase(&f, opaque ? "LISTEN FOR ECHO" : "LISTEN TO VOICE");
    for (unsigned recording = 0; recording < f.recording.count; ++recording) {
      uint64_t deadline = f.now + 10000;
      while (f.recording.played[recording] < f.recording.frames &&
             f.now < deadline) {
        advance(&f, f.now + 8);
        assert(service(&f) == 0 && !f.mfg.model.ready);
      }
      assert(f.recording.reads[recording] == f.recording.frames &&
             f.recording.accepted[recording] == f.recording.frames &&
             f.recording.played[recording] == f.recording.frames);
      for (unsigned strip = 0; strip < 24; ++strip) {
        advance(&f, f.now + 8);
        assert(service(&f) == 0 && !f.mfg.model.ready);
      }
      assert(controller_tick(&f) == 0);
      assert(f.mfg.model.ready == (!opaque || recording == 1));
    }
    unsigned reads = f.recording.reads[0];
    /* Passing the repeat deadline during rendering cannot begin a new cycle. */
    for (unsigned strip = 0; strip < 400; ++strip) {
      advance(&f, f.now + 8);
      assert(service(&f) == 0 && f.mfg.model.ready &&
             f.recording.reads[0] == reads);
    }
    assert(controller_tick(&f) == 0 && f.recording.reads[0] > reads);
    teardown(&f);
  }
}

static void service_errors_and_callback_overflow_remain_failures(void) {
  for (unsigned which = 0; which < 4; ++which) {
    fake_t f;
    init_slow_ui(&f, SAMPLES, 0, 1);
    assert(h2_mfg_hardware_enter(&f.mfg, 0) == 0);
    slow_ui_until_phase(&f, which == 1 ? "LISTEN TO VOICE" : "SPEAK OVER TONE");
    if (which == 0) f.recording.write_error = H2_PAL_ERR_IO;
    if (which == 1) f.recording.read_error = H2_PAL_ERR_IO;
    if (which == 2) {
      while (f.sequence < f.recording.frames) {
        advance(&f, f.now + 8);
        assert(service(&f) == 0);
      }
      /* Five complete measured frames overflow the four-frame callback queue.
       */
      advance(&f, f.now + 5 * SAMPLES / 16);
    }
    if (which == 3) f.observer.on_error(f.observer.user, 1, H2_PAL_ERR_IO);
    int rc = 0;
    uint64_t deadline = f.now + 10000;
    while (rc == 0 && f.now < deadline) {
      advance(&f, f.now + 8);
      rc = service(&f);
    }
    assert(rc == H2_PAL_ERR_IO && !f.mfg.model.ready &&
           h2_mfg_aec_tick(&f.mfg) == H2_PAL_ERR_IO);
    teardown(&f);
  }
}

int main(void) {
  slow_ui_with_two_playback_frames_keeps_complete_windows();
  unserviced_two_frame_playback_still_rejects_starvation();
  complete_capture_and_dac_are_required_before_finishing();
  service_exhaustion_and_repeat_wait_for_controller_tick();
  service_errors_and_callback_overflow_remain_failures();
  external_recordings_keep_all_pcm_with_small_memory();
  external_playback_backpressure_keeps_one_frame_loaded();
  external_retry_begins_a_fresh_voice_recording();
  external_begin_write_and_read_errors_cannot_pass();
  external_queue_overflow_and_final_drain_errors_cannot_pass();
  external_close_retains_dependencies_after_partial_open();
  external_allocation_failure_keeps_cleanup_available();
  external_paths_preserve_acoustic_and_transport_rejection();
  constrained_memory_keeps_voice_and_cleanup();
  operator_speech_and_replay_required();
  reject_missing_reference_echo_clipping_and_dac();
  muted_path_and_failed_cleanup_do_not_pass();
  board_frame_sizes_and_operator_acceptance();
  disabled_aec_and_observer_errors_cannot_fall_back();
  opaque_mute_clipping_and_transport_failures();
  return 0;
}
