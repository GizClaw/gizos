#include "../src/h2_mfg_internal.h"
#include "h2_desktop_platform.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
#include "h2_atomic.h"

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
      if (f->pending < MAX_QUEUE && !f->missing_capture) {
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

/* A real PAL task exercises monitor/controller ownership. The controller can
 * stay busy with its UI for 100 ms while 32-ms capture continues independently.
 * Provider configuration is immutable until successful join. */
typedef struct live_fake {
  h2_mfg_t m;
  h2_runtime_t rt;
  h2_pal_audio_api_t audio;
  h2_pal_task_api_t task;
  h2_mfg_profile_t profile;
  h2_audio_aec_observer_t observer;
  h2_pal_audio_track_t track;
  h2_atomic_uint_t reads, writes, blocks, gain_sets;
  h2_atomic_int_t fault;
  unsigned volume, capture_offset, output_offset, captured, played;
  uint64_t next_capture;
  int mic, speaker, silent, partial, disabled, bad_format;
  int start_error, join_error, close_error, unregister_error, restore_error;
  int create_error, observer_error, no_playback;
  h2_pal_task_t* handle;
} live_fake_t;

static int live_info(void* user, h2_audio_info_t* out) {
  live_fake_t* f = user;
  *out = (h2_audio_info_t){.available = 1,
                           .aec_enabled = !f->disabled,
                           .mic_supported = 1,
                           .playback_supported = 1,
                           .mic_format = format,
                           .playback_format = format};
  if (f->bad_format) out->mic_format.channels = 2;
  return 0;
}
static int live_volume_get(void* user, uint32_t* out) {
  *out = ((live_fake_t*)user)->volume;
  return 0;
}
static int live_volume_set(void* user, uint32_t v) {
  live_fake_t* f = user;
  if (v == 91 && f->restore_error) return H2_PAL_ERR_IO;
  f->volume = v;
  return 0;
}
static int live_gain_set(void* user, uint32_t v) {
  (void)v;
  (void)h2_atomic_uint_fetch_add(&((live_fake_t*)user)->gain_sets, 1u,
                                 H2_ATOMIC_SEQ_CST);
  return H2_PAL_ERR_IO;
}
static int live_observer(void* user, const h2_audio_aec_observer_t* o) {
  live_fake_t* f = user;
  assert(!f->mic && !f->speaker);
  if (o != NULL && f->observer_error) return H2_PAL_ERR_IO;
  if (o == NULL && f->unregister_error) return H2_PAL_ERR_BUSY;
  f->observer = o == NULL ? (h2_audio_aec_observer_t){0} : *o;
  return 0;
}
static int live_mic_start(void* user) {
  live_fake_t* f = user;
  f->mic = 1;
  return h2_pal_time_get_monotonic_ms(f->rt.time, &f->next_capture);
}
static int live_mic_stop(void* user) {
  ((live_fake_t*)user)->mic = 0;
  return 0;
}
static int live_speaker_start(void* user) {
  ((live_fake_t*)user)->speaker = 1;
  return 0;
}
static int live_speaker_stop(void* user) {
  ((live_fake_t*)user)->speaker = 0;
  return 0;
}
static int live_read(void* user, h2_audio_frame_t* out, uint32_t timeout) {
  live_fake_t* f = user;
  assert(f->mic && timeout == 0);
  int fault = h2_atomic_int_load(&f->fault, H2_ATOMIC_SEQ_CST);
  if (fault == 1) return H2_PAL_ERR_IO;
  if (fault == 2) return H2_PAL_ERR_WOULD_BLOCK;
  if (fault == 3) {
    f->observer.on_error(f->observer.user, 1, H2_PAL_ERR_INVALID_STATE);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  uint64_t now;
  assert(h2_pal_time_get_monotonic_ms(f->rt.time, &now) == 0);
  if (now < f->next_capture) return H2_PAL_ERR_WOULD_BLOCK;
  unsigned count = f->partial ? SAMPLES / 2 : SAMPLES;
  assert(out->capacity >= count * sizeof(int16_t));
  for (unsigned i = 0; i < count; ++i)
    ((int16_t*)out->data)[i] = f->silent ? 0 : (int16_t)(f->captured + 1);
  out->bytes = count * sizeof(int16_t);
  out->samples_per_channel = (uint16_t)count;
  if (fault == 4) out->channels = 2;
  f->capture_offset += count;
  if (f->capture_offset == SAMPLES) {
    f->capture_offset = 0;
    ++f->captured;
    f->next_capture += 32;
    (void)h2_atomic_uint_fetch_add(&f->reads, 1u, H2_ATOMIC_SEQ_CST);
  }
  return 0;
}
static int live_write(h2_pal_audio_track_t* t, const h2_audio_frame_t* in,
                      uint32_t timeout) {
  live_fake_t* f = t->user;
  assert(f->speaker && timeout == 0);
  int fault = h2_atomic_int_load(&f->fault, H2_ATOMIC_SEQ_CST);
  if (fault == 5) return H2_PAL_ERR_IO;
  if (fault == 6) return H2_PAL_ERR_WOULD_BLOCK;
  if (fault == 7) return 3; /* Invalid unaligned partial acceptance. */
  if (fault == 8) {
    f->observer.on_error(f->observer.user, 0, H2_PAL_ERR_FORMAT);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (f->partial &&
      h2_atomic_uint_fetch_add(&f->blocks, 1u, H2_ATOMIC_SEQ_CST) % 3 == 0)
    return H2_PAL_ERR_WOULD_BLOCK;
  size_t accepted = f->partial && in->bytes > 256 ? 256 : in->bytes;
  for (unsigned i = 0; i < accepted / sizeof(int16_t); ++i)
    assert(((const int16_t*)in->data)[i] ==
           (f->silent ? 0 : (int16_t)(f->played + 1)));
  f->output_offset += (unsigned)accepted;
  if (f->output_offset == SAMPLES * sizeof(int16_t)) {
    f->output_offset = 0;
    ++f->played;
    int16_t pcm[SAMPLES] = {0};
    h2_audio_frame_t frame =
        h2_audio_frame_for_buffer(pcm, sizeof(pcm), format);
    frame.bytes = sizeof(pcm);
    if (!f->no_playback) f->observer.on_playback(f->observer.user, &frame);
    (void)h2_atomic_uint_fetch_add(&f->writes, 1u, H2_ATOMIC_SEQ_CST);
  }
  return f->partial ? (int)accepted : 0;
}
static int live_close(h2_pal_audio_track_t* t) {
  live_fake_t* f = t->user;
  assert(f->handle == NULL); /* Join must precede any track teardown. */
  return f->close_error ? H2_PAL_ERR_BUSY : 0;
}
static int live_track(void* user, const h2_audio_track_config_t* c,
                      h2_pal_audio_track_t** out) {
  live_fake_t* f = user;
  assert(c->volume_factor_milli == 1000 && c->buffer_frames <= MAX_QUEUE);
  if (f->create_error) return H2_PAL_ERR_IO;
  f->track = (h2_pal_audio_track_t){
      .user = f, .write = live_write, .close = live_close};
  *out = &f->track;
  return 0;
}
static int live_task_start(void* user, const h2_pal_task_options_t* o,
                           h2_pal_task_entry_t entry, void* ctx,
                           h2_pal_task_t** out) {
  live_fake_t* f = user;
  assert(strcmp(o->name, "mfg/monitor") == 0);
  if (f->start_error) return H2_PAL_ERR_NO_MEMORY;
  int rc =
      h2_pal_task_start(h2_desktop_platform_task_api(), o, entry, ctx, out);
  if (rc == 0) f->handle = *out;
  return rc;
}
static int live_task_join(void* user, h2_pal_task_t* task) {
  live_fake_t* f = user;
  assert(f->handle == task);
  if (f->join_error) return H2_PAL_ERR_BUSY;
  int rc = h2_pal_task_join(h2_desktop_platform_task_api(), task);
  if (rc == 0) f->handle = NULL;
  return rc;
}
static const h2_pal_task_vtable_t live_tasks = {.start = live_task_start,
                                                .join = live_task_join};
static const h2_pal_audio_vtable_t live_audio = {
    .get_info = live_info,
    .get_speaker_volume_percent = live_volume_get,
    .set_speaker_volume_percent = live_volume_set,
    .set_mic_gain_percent = live_gain_set,
    .set_aec_observer = live_observer,
    .start_mic = live_mic_start,
    .stop_mic = live_mic_stop,
    .start_speaker = live_speaker_start,
    .stop_speaker = live_speaker_stop,
    .create_track = live_track,
    .mic_read = live_read};

static void live_setup(live_fake_t* f) {
  memset(f, 0, sizeof(*f));
  assert(h2_atomic_uint_init(&f->reads, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_uint_init(&f->writes, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_uint_init(&f->blocks, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_uint_init(&f->gain_sets, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_int_init(&f->fault, 0) == H2_ATOMIC_OK);
  f->volume = 91;
  f->profile = profile;
  f->profile.aec_live_monitor = 1;
  f->profile.speaker_test_volume = 50;
  f->audio = (h2_pal_audio_api_t){.user = f, .vtable = &live_audio};
  f->task = (h2_pal_task_api_t){.user = f, .vtable = &live_tasks};
  f->rt = (h2_runtime_t){.mem = h2_desktop_platform_default_allocator(),
                         .audio = &f->audio,
                         .task = &f->task,
                         .time = h2_desktop_platform_time_api(),
                         .sync = h2_desktop_platform_sync_api()};
  f->m = (h2_mfg_t){.rt = &f->rt,
                    .profile = &f->profile,
                    .model = {.profile = &f->profile, .summary = {.total = 1}}};
  const h2_pal_mutex_config_t mutex = {.name = "mfg/test-controller",
                                       .allocator = f->rt.mem};
  assert(h2_pal_mutex_create(f->rt.sync, &mutex, &f->m.mutex) == 0);
  /* UI and hardware lifecycle run under the controller lock in production.
   * Keep it held throughout, including join: the monitor must never need it. */
  assert(h2_pal_mutex_lock(f->rt.sync, f->m.mutex) == 0);
}
static void live_wait(live_fake_t* f, unsigned count) {
  for (unsigned tries = 0;
       h2_atomic_uint_load(&f->writes, H2_ATOMIC_SEQ_CST) < count &&
       tries < 500;
       ++tries)
    assert(h2_pal_time_sleep_ms(f->rt.time, 2) == 0);
  assert(h2_atomic_uint_load(&f->writes, H2_ATOMIC_SEQ_CST) >= count);
}
static void live_teardown(live_fake_t* f) {
  assert(h2_mfg_hardware_leave(&f->m) == 0);
  assert(f->handle == NULL && f->m.aec == NULL && f->m.pcm == NULL &&
         f->m.track == NULL && !f->mic && !f->speaker &&
         f->observer.user == NULL && f->volume == 91);
  assert(h2_atomic_uint_load(&f->gain_sets, H2_ATOMIC_SEQ_CST) == 0);
  h2_atomic_uint_destroy(&f->reads);
  h2_atomic_uint_destroy(&f->writes);
  h2_atomic_uint_destroy(&f->blocks);
  h2_atomic_uint_destroy(&f->gain_sets);
  h2_atomic_int_destroy(&f->fault);
  assert(h2_pal_mutex_unlock(f->rt.sync, f->m.mutex) == 0);
  assert(h2_pal_mutex_destroy(f->rt.sync, f->m.mutex) == 0);
}
static void live_silence_partial_and_slow_ui(void) {
  for (int silent = 0; silent < 2; ++silent) {
    live_fake_t f;
    live_setup(&f);
    f.silent = silent;
    f.partial = 1;
    f.profile.monitor_min_frames = silent ? 12 : 0;
    assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
    assert(f.volume == 50 && !f.m.model.ready);
    assert(strcmp(h2_mfg_aec_phase(&f.m), "LIVE MONITOR") == 0);
    live_wait(&f, 7);
    assert(h2_mfg_aec_tick(&f.m) == 0 && !f.m.model.ready);
    /* No controller ticks here: the independent task must keep moving PCM. */
    unsigned before = h2_atomic_uint_load(&f.writes, H2_ATOMIC_SEQ_CST);
    assert(h2_pal_time_sleep_ms(f.rt.time, 100) == 0);
    assert(h2_atomic_uint_load(&f.writes, H2_ATOMIC_SEQ_CST) >= before + 2);
    live_wait(&f, silent ? 12 : 8);
    assert(h2_mfg_aec_tick(&f.m) == 0 && f.m.model.ready);
    unsigned minimum = silent ? 12 : 8;
    assert(f.m.audio_frames >= minimum &&
           f.m.audio_samples >= minimum * SAMPLES);
    assert(strstr(f.m.detail, "captured=") && strstr(f.m.detail, "played="));
    assert(f.m.model.summary.step_status[0] == 0);
    live_teardown(&f);
  }
}
static void live_errors_and_retained_cleanup(void) {
  for (int fault = 1; fault <= 8; ++fault) {
    live_fake_t f;
    live_setup(&f);
    assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
    live_wait(&f, 8);
    assert(h2_mfg_aec_tick(&f.m) == 0 && f.m.model.ready);
    h2_atomic_int_store(&f.fault, fault, H2_ATOMIC_SEQ_CST);
    int rc = 0;
    for (unsigned tries = 0; rc == 0 && tries < 1300; ++tries) {
      assert(h2_pal_time_sleep_ms(f.rt.time, 2) == 0);
      rc = h2_mfg_aec_tick(&f.m);
    }
    int expected = fault == 2 || fault == 6   ? H2_PAL_ERR_TIMEOUT
                   : fault == 3               ? H2_PAL_ERR_INVALID_STATE
                   : fault == 4 || fault == 8 ? H2_PAL_ERR_FORMAT
                                              : H2_PAL_ERR_IO;
    assert(rc == expected && !f.m.model.ready);
    f.join_error = 1;
    void* aec = f.m.aec;
    void* pcm = f.m.pcm;
    assert(h2_mfg_hardware_leave(&f.m) == H2_PAL_ERR_BUSY);
    assert(f.m.aec == aec && f.m.pcm == pcm && f.m.track && f.handle);
    f.join_error = 0;
    f.close_error = 1;
    assert(h2_mfg_hardware_leave(&f.m) == H2_PAL_ERR_BUSY);
    assert(f.m.aec == aec && f.m.pcm == pcm && f.m.track && !f.handle);
    f.close_error = 0;
    f.unregister_error = 1;
    assert(h2_mfg_hardware_leave(&f.m) == H2_PAL_ERR_BUSY);
    assert(f.m.aec == aec && f.observer.user && f.m.pcm == pcm);
    f.unregister_error = 0;
    f.restore_error = 1;
    assert(h2_mfg_hardware_leave(&f.m) == H2_PAL_ERR_IO);
    assert(f.m.volume_saved && f.m.pcm == pcm && f.volume == 50);
    f.restore_error = 0;
    live_teardown(&f);
  }
}
static void live_start_failures(void) {
  for (unsigned which = 0; which < 6; ++which) {
    live_fake_t f;
    live_setup(&f);
    f.disabled = which == 0;
    f.bad_format = which == 1;
    f.observer_error = which == 2;
    f.create_error = which == 3;
    f.start_error = which == 4;
    if (which == 5) f.rt.task = NULL;
    assert(h2_mfg_hardware_enter(&f.m, 0) != 0 && !f.m.model.ready);
    live_teardown(&f);
  }
  live_fake_t f;
  live_setup(&f);
  f.no_playback = 1;
  assert(h2_mfg_hardware_enter(&f.m, 0) == 0);
  int rc = 0;
  for (unsigned tries = 0; rc == 0 && tries < 1300; ++tries) {
    assert(h2_pal_time_sleep_ms(f.rt.time, 2) == 0);
    rc = h2_mfg_aec_tick(&f.m);
    assert(!f.m.model.ready);
  }
  assert(rc == H2_PAL_ERR_TIMEOUT);
  live_teardown(&f);
}

int main(void) {
  live_silence_partial_and_slow_ui();
  live_errors_and_retained_cleanup();
  live_start_failures();
  operator_speech_and_replay_required();
  reject_missing_reference_echo_clipping_and_dac();
  muted_path_and_failed_cleanup_do_not_pass();
  board_frame_sizes_and_operator_acceptance();
  disabled_aec_and_observer_errors_cannot_fall_back();
  opaque_mute_clipping_and_transport_failures();
  return 0;
}
