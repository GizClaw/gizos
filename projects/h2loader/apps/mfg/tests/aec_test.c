#include "../src/h2_mfg_internal.h"
#include "h2_desktop_platform.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SAMPLES = 512, MAX_QUEUE = 8 };
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
  unsigned capture_capacity, button_delay_ms, button_reads, button_pressed;
  unsigned ui_delay_ms, ui_frames;
  int speaker, mic, human_speech, no_echo_cancel, missing_reference, muted;
  int clipping, close_error, unregister_error, missing_dac;
  int opaque, disabled, register_error, partial_reads;
  int output_clipping, missing_capture, drain_error;
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
  assert(!f->speaker && !f->mic);
  if (f->opaque) return H2_PAL_ERR_UNSUPPORTED;
  if (o != NULL && f->register_error) return H2_PAL_ERR_IO;
  if (o == NULL && f->unregister_error) return H2_PAL_ERR_BUSY;
  f->observer = o == NULL ? (h2_audio_aec_observer_t){0} : *o;
  return 0;
}
static int start_mic(void* user) {
  fake_t* f = user;
  f->mic = 1;
  f->pending = 0;
  f->mic_head = f->mic_offset = 0;
  f->sequence = 0;
  f->next = f->now + f->frame_samples / 16;
  return 0;
}
static int stop_mic(void* user) {
  ((fake_t*)user)->mic = 0;
  return 0;
}
static int start_speaker(void* user) {
  fake_t* f = user;
  f->speaker = 1;
  f->next = f->now + f->frame_samples / 16;
  return 0;
}
static int stop_speaker(void* user) {
  fake_t* f = user;
  assert(f->capacity == 0);
  f->speaker = 0;
  return 0;
}
static int close_track(h2_pal_audio_track_t* t) {
  fake_t* f = t->user;
  if (f->close_error) return H2_PAL_ERR_BUSY;
  f->capacity = f->count = 0;
  return 0;
}
static int write_frame(h2_pal_audio_track_t* t, const h2_audio_frame_t* frame,
                       uint32_t timeout) {
  fake_t* f = t->user;
  assert(timeout == 0 && f->speaker);
  assert(frame->bytes == f->frame_samples * sizeof(int16_t));
  if (f->count == f->capacity) return H2_PAL_ERR_WOULD_BLOCK;
  memcpy(f->queue[(f->head + f->count) % MAX_QUEUE], frame->data, frame->bytes);
  ++f->count;
  return 0;
}
static void advance(fake_t* f, uint64_t target);
static int drain(h2_pal_audio_track_t* t, uint32_t timeout) {
  fake_t* f = t->user;
  assert(timeout == 1000);
  if (f->drain_error) return H2_PAL_ERR_IO;
  advance(f, f->now + f->count * f->frame_samples / 16);
  return 0;
}
static int create_track(void* user, const h2_audio_track_config_t* c,
                        h2_pal_audio_track_t** out) {
  fake_t* f = user;
  assert(c->buffer_frames <= MAX_QUEUE);
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
    if (f->count != 0) {
      memcpy(pcm, f->queue[f->head], f->frame_samples * sizeof(int16_t));
      f->head = (f->head + 1) % MAX_QUEUE;
      --f->count;
    }
    if (f->observer.on_playback != NULL && !f->missing_dac) {
      h2_audio_frame_t frame =
          h2_audio_frame_for_buffer(pcm, sizeof(pcm), pcm_format(f));
      frame.bytes = f->frame_samples * sizeof(int16_t);
      f->observer.on_playback(f->observer.user, &frame);
    } else if (!f->mic) {
      ++f->replay_frames;
      if (strcmp(h2_mfg_aec_phase(&f->mfg), "LISTEN FOR ECHO") == 0)
        ++f->echo_replay_frames;
    }
    if (f->mic) {
      const char* phase = h2_mfg_aec_phase(&f->mfg);
      int speech = f->human_speech && (strcmp(phase, "SPEAK NOW") == 0 ||
                                       strcmp(phase, "SPEAK OVER TONE") == 0);
      for (unsigned i = 0; i < f->frame_samples; ++i) {
        int16_t voice = speech ? (i % 2 ? -1024 : 1024) : 0;
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
      if (f->observer.on_capture != NULL)
        f->observer.on_capture(f->observer.user, &frame);
      if (f->pending < f->capture_capacity && !f->missing_capture) {
        memcpy(f->captured[(f->mic_head + f->pending) % MAX_QUEUE], out,
               f->frame_samples * sizeof(int16_t));
        ++f->pending;
      }
    }
  }
  f->now = target;
}

static void setup_format(fake_t* f, unsigned samples, int opaque) {
  memset(f, 0, sizeof(*f));
  f->frame_samples = samples;
  f->capture_capacity = MAX_QUEUE;
  f->opaque = opaque;
  f->volume = 91;
  f->audio = (h2_pal_audio_api_t){.user = f, .vtable = &audio_vtable};
  f->time = (h2_pal_time_api_t){.user = f, .vtable = &time_vtable};
  f->rt = (h2_runtime_t){.mem = h2_desktop_platform_default_allocator(),
                         .audio = &f->audio,
                         .time = &f->time};
  f->mfg = (h2_mfg_t){.rt = &f->rt,
                      .profile = &profile,
                      .model = {.profile = &profile, .summary = {.total = 1}}};
  assert(h2_mfg_hardware_enter(&f->mfg, 0) == 0);
}

static void setup(fake_t* f) { setup_format(f, SAMPLES, 0); }

static int tick(fake_t* f) {
  advance(f, f->now + (f->opaque ? 20 : 125));
  const char* action = NULL;
  int rc = h2_mfg_hardware_tick(&f->mfg, f->now, &action);
  if (rc == 0 && f->ui_delay_ms && f->now - f->mfg.last_draw_ms >= 100) {
    f->mfg.last_draw_ms = f->now;
    rc = h2_mfg_ui_draw(&f->mfg, f->now);
  }
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

static h2_pal_result_t slow_button_group(
    void* user, h2_pal_periph_id_t id,
    h2_pal_radio_button_group_reading_t* out) {
  fake_t* f = user;
  assert(id == 100);
  ++f->button_reads;
  advance(f, f->now + f->button_delay_ms);
  *out = (h2_pal_radio_button_group_reading_t){
      .id = id, .pressed_button_id = f->button_pressed ? 1u : 0u};
  return H2_PAL_OK;
}

static void blocking_buttons_do_not_starve_aec(void) {
  fake_t f;
  setup_format(&f, 320, 1);
  /* Four 20ms microphone buffers cannot cover a blocking 140ms ADC poll. */
  f.capture_capacity = 4;
  f.button_delay_ms = 140;
  f.button_pressed = 1;
  const h2_pal_button_vtable_t vt = {.read_radio_button_group =
                                         slow_button_group};
  const h2_pal_button_api_t button = {.user = &f, .vtable = &vt};
  h2_mfg_profile_t configured = profile;
  configured.button_group = 100;
  configured.buttons[0] = 1;
  configured.button_count = 1;
  f.rt.button = &button;
  f.mfg.profile = f.mfg.model.profile = &configured;
  while (f.now < 25000) {
    int rc = tick(&f);
    if (rc != 0)
      fprintf(stderr, "AEC window failed: rc=%d button_reads=%u time_ms=%llu\n",
              rc, f.button_reads, (unsigned long long)f.now);
    assert(rc == 0 && !f.mfg.model.ready);
  }
  assert(strcmp(h2_mfg_aec_phase(&f.mfg), "SPEAK NOW") == 0);
  assert(f.button_reads == 0 && f.replay_frames == 0);
  f.human_speech = 1;
  while (!f.mfg.model.ready && f.now < 65000) assert(tick(&f) == 0);
  assert(f.mfg.model.ready && f.button_reads == 0);
  assert(f.echo_replay_frames >= 154 && f.replay_frames >= 308);

  /* A key held through capture cannot accept the later listening result. */
  const char* action = NULL;
  assert(h2_mfg_hardware_tick(&f.mfg, f.now, &action) == 0 && action == NULL);
  f.button_pressed = 0;
  assert(h2_mfg_hardware_tick(&f.mfg, f.now, &action) == 0 && action == NULL);
  /* A fresh press/release must still support listening acceptance and retry. */
  f.button_pressed = 1;
  assert(h2_mfg_hardware_tick(&f.mfg, f.now, &action) == 0 && action == NULL);
  f.button_pressed = 0;
  assert(h2_mfg_hardware_tick(&f.mfg, f.now, &action) == 0);
  assert(action != NULL && strcmp(action, "confirm") == 0);
  f.mfg.model.error = H2_PAL_ERR_INVALID_STATE;
  f.mfg.model.ready = 0;
  f.mfg.model.summary.step_status[0] = H2_LOADER_MFG_STEP_FAILED;
  action = NULL;
  f.button_pressed = 1;
  assert(h2_mfg_hardware_tick(&f.mfg, f.now, &action) == 0 && action == NULL);
  f.button_pressed = 0;
  assert(h2_mfg_hardware_tick(&f.mfg, f.now, &action) == 0);
  assert(action != NULL && strcmp(action, "retry") == 0);
  assert(f.button_reads == 6);
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

static int slow_ui_present(void* user) {
  fake_t* f = user;
  ++f->ui_frames;
  advance(f, f->now + f->ui_delay_ms);
  return H2_PAL_OK;
}

static void blocking_display_does_not_starve_aec(void) {
  fake_t f;
  setup_format(&f, 320, 1);
  f.capture_capacity = 4;
  f.ui_delay_ms = 140;
  const h2_pal_display_vtable_t vt = {.draw_bitmap = draw_failure,
                                      .present = slow_ui_present};
  const h2_pal_display_api_t display = {.user = &f, .vtable = &vt};
  const h2_mfg_ui_config_t ui = {.width = 480,
                                 .height = 480,
                                 .text = 0xffff,
                                 .rows = {{48, 144, 384, 30},
                                          {48, 185, 384, 30},
                                          {48, 225, 384, 30},
                                          {48, 265, 384, 30}}};
  h2_mfg_profile_t configured = profile;
  configured.ui = &ui;
  f.mfg.profile = f.mfg.model.profile = &configured;
  f.rt.display = &display;
  f.mfg.display_open = 1;
  f.mfg.display.width = f.mfg.display.height = 480;
  f.mfg.frame = h2_pal_mem_alloc(f.rt.mem, 480u * 480u * sizeof(uint16_t));
  assert(f.mfg.frame != NULL);
  while (f.now < 25000) {
    int rc = tick(&f);
    if (rc != 0)
      fprintf(stderr, "AEC UI starvation: rc=%d ui_frames=%u time_ms=%llu\n",
              rc, f.ui_frames, (unsigned long long)f.now);
    assert(rc == 0 && !f.mfg.model.ready);
  }
  assert(strcmp(h2_mfg_aec_phase(&f.mfg), "SPEAK NOW") == 0);
  f.human_speech = 1;
  while (!f.mfg.model.ready && f.now < 65000) assert(tick(&f) == 0);
  assert(f.mfg.model.ready && f.echo_replay_frames >= 154);
  assert(f.replay_frames >= 308);
  assert(f.ui_frames >= 7 && f.ui_frames <= 9);
  h2_pal_mem_free(f.rt.mem, f.mfg.frame);
  f.mfg.frame = NULL;
  f.mfg.display_open = 0;
  teardown(&f);
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

typedef struct recording_heap {
  size_t limit;
  unsigned attempts, large_attempts, live, frees;
} recording_heap_t;

typedef struct recording_allocation {
  recording_heap_t* owner;
  size_t bytes;
  max_align_t alignment;
} recording_allocation_t;

static void* recording_allocate(void* user, size_t bytes) {
  recording_heap_t* heap = user;
  ++heap->attempts;
  if (bytes > 65536u) ++heap->large_attempts;
  if (bytes > heap->limit) return NULL;
  assert(bytes <= SIZE_MAX - sizeof(recording_allocation_t));
  recording_allocation_t* allocation = malloc(sizeof(*allocation) + bytes);
  assert(allocation);
  allocation->owner = heap;
  allocation->bytes = bytes;
  ++heap->live;
  return allocation + 1;
}

static void recording_release(void* user, void* pointer) {
  recording_heap_t* heap = user;
  recording_allocation_t* allocation = (recording_allocation_t*)pointer - 1;
  assert(allocation->owner == heap && heap->live != 0u);
  if (allocation->bytes > 65536u) {
    const unsigned char* bytes = pointer;
    for (size_t i = 0; i < allocation->bytes; ++i) assert(bytes[i] == 0);
  }
  --heap->live;
  ++heap->frees;
  free(allocation);
}

static void recording_memory_placement(void) {
  const h2_pal_mem_vtable_t memory = {.alloc = recording_allocate,
                                      .free = recording_release};
  for (unsigned scenario = 0; scenario < 4; ++scenario) {
    fake_t f;
    setup_format(&f, 320, 1);
    teardown(&f);
    recording_heap_t external = {.limit = scenario == 0 ? SIZE_MAX : 0};
    recording_heap_t ordinary = {
        .limit = scenario == 0 || scenario == 3 ? 65536u : SIZE_MAX};
    const h2_pal_mem_api_t external_api = {.user = &external,
                                           .vtable = &memory};
    const h2_pal_mem_api_t ordinary_api = {.user = &ordinary,
                                           .vtable = &memory};
    f.rt.mem = &ordinary_api;
    f.rt.mem_external = scenario == 2 ? NULL : &external_api;
    const int rc = h2_mfg_hardware_enter(&f.mfg, f.now);
    if (scenario == 3) {
      assert(rc == H2_PAL_ERR_NO_MEMORY && f.mfg.aec == NULL);
      assert(!f.mfg.model.ready && !f.speaker && !f.mic);
      assert(strstr(f.mfg.detail, "recording buffer") != NULL);
    } else {
      assert(rc == H2_PAL_OK && f.mfg.aec != NULL);
      if (scenario == 0) {
        while (f.now < 25000) assert(tick(&f) == 0 && !f.mfg.model.ready);
        f.human_speech = 1;
        while (!f.mfg.model.ready && f.now < 65000) assert(tick(&f) == 0);
        assert(f.mfg.model.ready && f.echo_replay_frames != 0);
        f.close_error = 1;
        assert(h2_mfg_hardware_leave(&f.mfg) == H2_PAL_ERR_BUSY);
        assert(external.live == 1 && ordinary.live == 1);
        f.close_error = 0;
      }
    }
    assert(external.large_attempts == (scenario == 2 ? 0u : 1u));
    assert(ordinary.large_attempts == (scenario == 0 ? 0u : 1u));
    teardown(&f);
    assert(external.live == 0 && ordinary.live == 0);
    assert(external.frees == (scenario == 0 ? 1u : 0u));
  }
}

int main(void) {
  recording_memory_placement();
  operator_speech_and_replay_required();
  blocking_buttons_do_not_starve_aec();
  blocking_display_does_not_starve_aec();
  reject_missing_reference_echo_clipping_and_dac();
  muted_path_and_failed_cleanup_do_not_pass();
  board_frame_sizes_and_operator_acceptance();
  disabled_aec_and_observer_errors_cannot_fall_back();
  opaque_mute_clipping_and_transport_failures();
  return 0;
}
