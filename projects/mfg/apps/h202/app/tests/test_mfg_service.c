#define _POSIX_C_SOURCE 200809L
#include "h2_h202_mfg.h"
#include "h2_loader_boot.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Pure measurement and playback helpers --------------------------- */

static const h2_h202_mfg_aec_config_t aec = {.sample_rate_hz = 16000u,
                                             .convergence_ms = 3000u,
                                             .measure_ms = 7000u,
                                             .block_ms = 100u};

typedef struct signal {
  float raw[2]; /**< Speech-like amplitude per lane, full scale 1.0. */
  float reference;
  float processed;
  uint64_t raw_until; /**< Raw lanes go silent from this sample; 0 = never. */
} signal_t;

static int16_t pcm(float x) {
  x *= 32767.0f;
  return (int16_t)(x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : x);
}

/* Feeds frames for one lane exactly as the H202 BSP observer delivers them. */
static void feed(h2_h202_mfg_aec_meter_t* meter, const signal_t* s,
                 uint8_t lane, unsigned frames) {
  static int16_t raw[320 * 3], reference[320], processed[320];
  uint64_t t = 0;
  for (unsigned f = 0; f < frames; ++f) {
    for (unsigned i = 0; i < 320u; ++i, ++t) {
      float wave = sinf(2.0f * 3.14159265f * 300.0f * (float)t / 16000.0f);
      bool live = s->raw_until == 0u || t < s->raw_until;
      for (unsigned l = 0; l < 2u; ++l)
        raw[i * 3u + l] = live ? pcm(s->raw[l] * wave) : 0;
      raw[i * 3u + 2u] = reference[i] = pcm(s->reference * wave);
      processed[i] = pcm(s->processed * wave);
    }
    h2_audio_frame_t r = {.data = raw,
                          .capacity = sizeof(raw),
                          .bytes = sizeof(raw),
                          .sample_rate_hz = 16000u,
                          .samples_per_channel = 320u,
                          .channels = 3u,
                          .sample_format = H2_AUDIO_SAMPLE_S16LE};
    h2_audio_frame_t ref = {.data = reference,
                            .capacity = sizeof(reference),
                            .bytes = sizeof(reference),
                            .sample_rate_hz = 16000u,
                            .samples_per_channel = 320u,
                            .channels = 1u,
                            .sample_format = H2_AUDIO_SAMPLE_S16LE};
    h2_audio_frame_t out = ref;
    out.data = processed;
    const h2_audio_aec_frame_t frame = {.sequence = f,
                                        .raw = &r,
                                        .reference_input = &ref,
                                        .processed = &out,
                                        .microphone_lane = lane,
                                        .reference_lane = 2u,
                                        .microphone_mask = 3u};
    h2_h202_mfg_aec_meter_capture(meter, &frame);
  }
}

static h2_h202_mfg_aec_verdict_t measure(const signal_t* s, uint8_t lane,
                                         uint8_t fed_lane, unsigned frames,
                                         h2_h202_mfg_aec_result_t* result) {
  static h2_h202_mfg_aec_meter_t meter;
  assert(h2_h202_mfg_aec_meter_init(&meter, &aec, lane) == H2_PAL_OK);
  feed(&meter, s, fed_lane, frames);
  return h2_h202_mfg_aec_meter_result(&meter, result);
}

/* Stage 05 checks signal presence only and reports broadband levels; there
 * is no tone and no echo-reduction verdict. */
static void test_measurement(void) {
  h2_h202_mfg_aec_result_t r;
  const signal_t good = {.raw = {0.2f, 0.2f}, .processed = 0.1f};
  assert(measure(&good, 0, 0, 500, &r) == H2_H202_MFG_AEC_PASS);
  assert(r.blocks == 70u);
  assert(r.raw_dbfs_x10 > -175 && r.raw_dbfs_x10 < -165); /* 0.2 sine: -17 */
  assert(r.processed_dbfs_x10 > -235 && r.processed_dbfs_x10 < -225);
  assert(r.reference_dbfs_x10 == -1200);
  assert(measure(&good, 1, 1, 500, &r) == H2_H202_MFG_AEC_PASS);
  /* Silent lane: all-zero raw signal. */
  signal_t silent = good;
  silent.raw[1] = 0.0f;
  assert(measure(&silent, 1, 1, 500, &r) == H2_H202_MFG_AEC_NO_SIGNAL);
  /* No complete frame for this lane. */
  assert(measure(&good, 1, 0, 500, &r) == H2_H202_MFG_AEC_NO_SIGNAL);
  /* Signal only during convergence does not count. */
  signal_t early = good;
  early.raw_until = 16000u * 2u;
  assert(measure(&early, 0, 0, 500, &r) == H2_H202_MFG_AEC_NO_SIGNAL);
  /* No AEC output. */
  signal_t dead = good;
  dead.processed = 0.0f;
  assert(measure(&dead, 0, 0, 500, &r) == H2_H202_MFG_AEC_NO_SIGNAL);
  /* Window not finished; convergence frames are never measured. */
  assert(measure(&good, 0, 0, 300, &r) == H2_H202_MFG_AEC_INCOMPLETE);
  assert(measure(&good, 0, 0, 100, &r) == H2_H202_MFG_AEC_NO_SIGNAL);
  h2_h202_mfg_aec_config_t bad = aec;
  bad.block_ms = 50u;
  h2_h202_mfg_aec_meter_t meter;
  assert(h2_h202_mfg_aec_meter_init(&meter, &bad, 0) == H2_PAL_ERR_INVALID_ARG);
  puts("aec measurement ok");
}

typedef struct track_fake {
  h2_pal_audio_track_t track;
  int16_t frames[16][320];
  unsigned writes;
  unsigned drains;
  int fail_at;
  int cancel_at;
} track_fake_t;

static int track_write(h2_pal_audio_track_t* track,
                       const h2_audio_frame_t* frame, uint32_t timeout_ms) {
  (void)timeout_ms;
  track_fake_t* t = track->user;
  assert(frame->sample_rate_hz == 16000u && frame->channels == 1u);
  assert(frame->samples_per_channel == 320u && frame->bytes == 640u);
  if ((int)t->writes == t->fail_at) return H2_PAL_ERR_IO;
  assert(t->writes < 16u);
  memcpy(t->frames[t->writes++], frame->data, 640u);
  return H2_PAL_OK;
}

static int track_drain(h2_pal_audio_track_t* track, uint32_t timeout_ms) {
  (void)timeout_ms;
  ++((track_fake_t*)track->user)->drains;
  return H2_PAL_OK;
}

static bool track_canceled(void* user) {
  track_fake_t* t = user;
  return t->cancel_at >= 0 && (int)t->writes >= t->cancel_at;
}

static void track_init(track_fake_t* t) {
  memset(t, 0, sizeof(*t));
  t->track = (h2_pal_audio_track_t){
      .user = t, .write = track_write, .drain = track_drain};
  t->fail_at = -1;
  t->cancel_at = -1;
}

static void test_prompt_playback(void) {
  int16_t in[3] = {0, 100, 200}, out[6], state = 0;
  h2_h202_mfg_upsample(in, 3, out, &state);
  const int16_t expected[6] = {0, 0, 50, 100, 150, 200};
  assert(memcmp(out, expected, sizeof(out)) == 0 && state == 200);
  /* Reference values from prompts/ima_adpcm.py: low nibble first, a zero
   * step for the first codes, sign bit 8, and index growth by 8. */
  h2_h202_mfg_adpcm_state_t decoder = {0};
  const unsigned char codes[3] = {0x08, 0x80, 0x7f};
  const int16_t decoded[6] = {0, 0, 0, 0, -11, 19};
  for (unsigned i = 0; i < 6u; ++i)
    assert(h2_h202_mfg_adpcm_next(
               &decoder, (codes[i / 2u] >> (i % 2u * 4u)) & 15u) == decoded[i]);
  /* 125 bytes, 250 samples at 8 kHz: one whole frame and a zero-padded
   * partial one; code 7 ramps 11, 41, 104 and saturates. */
  static unsigned char bytes[125];
  memset(bytes, 0x77, sizeof(bytes));
  track_fake_t t;
  track_init(&t);
  assert(h2_h202_mfg_play_adpcm8k(&t.track, bytes, sizeof(bytes), NULL, NULL) ==
         H2_PAL_OK);
  assert(t.writes == 2u && t.drains == 1u);
  assert(t.frames[0][0] == 11 / 2 && t.frames[0][1] == 11);
  assert(t.frames[0][2] == (11 + 41) / 2 && t.frames[0][5] == 104);
  assert(t.frames[0][319] == INT16_MAX && t.frames[1][0] == INT16_MAX);
  assert(t.frames[1][179] == INT16_MAX && t.frames[1][180] == 0);
  assert(t.frames[1][319] == 0);
  /* Track write failure is the playback error; no drain follows. */
  track_init(&t);
  t.fail_at = 1;
  assert(h2_h202_mfg_play_adpcm8k(&t.track, bytes, sizeof(bytes), NULL, NULL) ==
         H2_PAL_ERR_IO);
  assert(t.drains == 0u);
  track_init(&t);
  t.cancel_at = 1;
  assert(h2_h202_mfg_play_adpcm8k(&t.track, bytes, sizeof(bytes),
                                  track_canceled, &t) == H2_PAL_ERR_CLOSED);
  assert(t.writes == 1u);
  assert(h2_h202_mfg_play_adpcm8k(&t.track, NULL, 1u, NULL, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  puts("prompt decoding and framing ok");
}

/* Five notes in turn, each followed by true silence, faded at both edges,
 * at the configured pitch, repeating from the first note. */
static void test_scale(void) {
  /* The H106 Zero BK melody: gapless, one continuous phase across notes. */
  static const h2_h202_mfg_scale_config_t melody = {
      .sample_rate_hz = 16000u,
      .notes_hz = {523u, 659u, 784u, 659u, 587u, 698u, 880u, 698u},
      .note_ms = 400u,
      .gap_ms = 0u,
      .amplitude = 10922};
  h2_h202_mfg_scale_t scale;
  assert(h2_h202_mfg_scale_init(&scale, &melody) == H2_PAL_OK);
  int previous = 0;
  for (unsigned round = 0; round < 2u; ++round) {
    for (unsigned n = 0; n < H2_H202_MFG_SCALE_NOTES; ++n) {
      int peak = 0, crossings = 0, step = 0;
      for (unsigned i = 0; i < 6400u; ++i) {
        int v = h2_h202_mfg_scale_next(&scale);
        if (abs(v) > peak) peak = abs(v);
        if ((previous < 0 && v >= 0) || (previous > 0 && v <= 0)) ++crossings;
        /* No jump at note edges: at most one step of the highest note. */
        if (abs(v - previous) > step) step = abs(v - previous);
        previous = v;
      }
      assert(peak > 10800 && peak <= 10922 && step < 3800);
      int expected = (int)(2u * melody.notes_hz[n] * 4u / 10u);
      assert(abs(crossings - expected) <= 2);
    }
  }
  /* With a gap, every note fades in and out and the gap is silent. */
  static const h2_h202_mfg_scale_config_t config = {
      .sample_rate_hz = 16000u,
      .notes_hz = {523u, 587u, 659u, 784u, 880u, 523u, 587u, 659u},
      .note_ms = 300u,
      .gap_ms = 150u,
      .amplitude = 24000};
  assert(h2_h202_mfg_scale_init(&scale, &config) == H2_PAL_OK);
  for (unsigned n = 0; n < H2_H202_MFG_SCALE_NOTES; ++n) {
    int peak = 0, last = 0;
    int first = h2_h202_mfg_scale_next(&scale);
    for (unsigned i = 1; i < 4800u; ++i) {
      last = h2_h202_mfg_scale_next(&scale);
      if (abs(last) > peak) peak = abs(last);
    }
    assert(first == 0 && abs(last) < 600 && peak > 23000);
    for (unsigned i = 0; i < 2400u; ++i)
      assert(h2_h202_mfg_scale_next(&scale) == 0);
  }
  h2_h202_mfg_scale_config_t bad = melody;
  bad.notes_hz[2] = 0u;
  assert(h2_h202_mfg_scale_init(&scale, &bad) == H2_PAL_ERR_INVALID_ARG);
  bad = melody;
  bad.notes_hz[7] = 8000u;
  assert(h2_h202_mfg_scale_init(&scale, &bad) == H2_PAL_ERR_INVALID_ARG);
  bad = melody;
  bad.note_ms = 0u;
  assert(h2_h202_mfg_scale_init(&scale, &bad) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_h202_mfg_scale_init(NULL, &melody) == H2_PAL_ERR_INVALID_ARG);
  puts("aging melody ok");
}

/* ---- Threaded service fixture ---------------------------------------- */

struct h2_pal_task {
  pthread_t native;
  h2_pal_task_entry_t entry;
  void* user;
};
struct h2_pal_mutex {
  pthread_mutex_t native;
};
struct h2_pal_semaphore {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  unsigned count, max;
};

static atomic_int tasks;
static atomic_uint key_mask;
static atomic_bool usb, chrg, done_line, white, card, silent_right;
static atomic_bool wifi_connect_locked, shutdown_locked;
static atomic_bool shutdown_key_down;
static atomic_uint shutdowns, wifi_connects, led_frames, mic_starts;
static atomic_uint reboots, speaker_volume;
static atomic_bool reboot_locked;
static atomic_int last_color; /* Packed RGB of pixel 3. */
static h2_pal_mutex_t* operation;
static h2_runtime_t runtime;

static void sleep_ms(unsigned ms) {
  struct timespec t = {.tv_sec = ms / 1000u,
                       .tv_nsec = (long)(ms % 1000u) * 1000000L};
  while (nanosleep(&t, &t) != 0) assert(errno == EINTR);
}

static void* mem_alloc(void* user, size_t n) {
  (void)user;
  return calloc(1, n);
}
static void mem_free(void* user, void* p) {
  (void)user;
  free(p);
}
static const h2_pal_mem_vtable_t mem_vtable = {.alloc = mem_alloc,
                                               .free = mem_free};
static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};

static h2_pal_result_t now_ms(void* user, uint64_t* out) {
  (void)user;
  struct timespec t;
  assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
  *out = (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
  return H2_PAL_OK;
}
static h2_pal_result_t sleep_cb(void* user, uint32_t ms) {
  (void)user;
  sleep_ms(ms);
  return H2_PAL_OK;
}
static const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms = now_ms,
                                                 .sleep_ms = sleep_cb};
static const h2_pal_time_api_t time_api = {.vtable = &time_vtable};

static void* task_main(void* user) {
  h2_pal_task_t* task = user;
  task->entry(task->user);
  return NULL;
}
static int task_start(void* user, const h2_pal_task_options_t* options,
                      h2_pal_task_entry_t entry, void* ctx,
                      h2_pal_task_t** out) {
  (void)user;
  assert(strncmp(options->name, "h202/mfg", 8) == 0);
  h2_pal_task_t* task = calloc(1, sizeof(*task));
  task->entry = entry;
  task->user = ctx;
  atomic_fetch_add(&tasks, 1);
  assert(pthread_create(&task->native, NULL, task_main, task) == 0);
  *out = task;
  return H2_PAL_OK;
}
static int task_join(void* user, h2_pal_task_t* task) {
  (void)user;
  assert(pthread_join(task->native, NULL) == 0);
  atomic_fetch_sub(&tasks, 1);
  free(task);
  return H2_PAL_OK;
}
static const h2_pal_task_vtable_t task_vtable = {.start = task_start,
                                                 .join = task_join};
static const h2_pal_task_api_t task_api = {.vtable = &task_vtable};

static h2_pal_result_t mutex_create(void* user, const h2_pal_mutex_config_t* c,
                                    h2_pal_mutex_t** out) {
  (void)user;
  (void)c;
  *out = calloc(1, sizeof(**out));
  assert(pthread_mutex_init(&(*out)->native, NULL) == 0);
  return H2_PAL_OK;
}
static h2_pal_result_t mutex_destroy(void* user, h2_pal_mutex_t* m) {
  (void)user;
  assert(pthread_mutex_destroy(&m->native) == 0);
  free(m);
  return H2_PAL_OK;
}
static h2_pal_result_t mutex_lock(void* user, h2_pal_mutex_t* m) {
  (void)user;
  return pthread_mutex_lock(&m->native) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}
static h2_pal_result_t mutex_try(void* user, h2_pal_mutex_t* m) {
  (void)user;
  return pthread_mutex_trylock(&m->native) == 0 ? H2_PAL_OK
                                                : H2_PAL_ERR_WOULD_BLOCK;
}
static h2_pal_result_t mutex_unlock(void* user, h2_pal_mutex_t* m) {
  (void)user;
  return pthread_mutex_unlock(&m->native) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}
static h2_pal_result_t sem_create(void* user,
                                  const h2_pal_semaphore_config_t* c,
                                  h2_pal_semaphore_t** out) {
  (void)user;
  h2_pal_semaphore_t* s = calloc(1, sizeof(*s));
  pthread_mutex_init(&s->lock, NULL);
  pthread_cond_init(&s->changed, NULL);
  s->count = c->initial_count;
  s->max = c->max_count;
  *out = s;
  return H2_PAL_OK;
}
static h2_pal_result_t sem_destroy(void* user, h2_pal_semaphore_t* s) {
  (void)user;
  pthread_cond_destroy(&s->changed);
  pthread_mutex_destroy(&s->lock);
  free(s);
  return H2_PAL_OK;
}
static h2_pal_result_t sem_take(void* user, h2_pal_semaphore_t* s,
                                uint32_t timeout_ms) {
  (void)user;
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_ms / 1000u;
  deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(&s->lock);
  int rc = 0;
  while (s->count == 0u && rc == 0)
    rc = pthread_cond_timedwait(&s->changed, &s->lock, &deadline);
  bool taken = s->count != 0u;
  if (taken) --s->count;
  pthread_mutex_unlock(&s->lock);
  return taken ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}
static h2_pal_result_t sem_give(void* user, h2_pal_semaphore_t* s) {
  (void)user;
  pthread_mutex_lock(&s->lock);
  bool full = s->count >= s->max;
  if (!full) ++s->count;
  pthread_cond_signal(&s->changed);
  pthread_mutex_unlock(&s->lock);
  return full ? H2_PAL_ERR_FULL : H2_PAL_OK;
}
static const h2_pal_sync_vtable_t sync_vtable = {
    .create_mutex = mutex_create,
    .destroy_mutex = mutex_destroy,
    .lock_mutex = mutex_lock,
    .try_lock_mutex = mutex_try,
    .unlock_mutex = mutex_unlock,
    .create_semaphore = sem_create,
    .destroy_semaphore = sem_destroy,
    .take_semaphore = sem_take,
    .give_semaphore = sem_give};
static const h2_pal_sync_api_t sync_api = {.vtable = &sync_vtable};

/* The operation mutex is not recursive here, so try_lock from the worker
 * itself proves the worker holds it. */
static bool operation_held(void) {
  if (mutex_try(NULL, operation) != H2_PAL_OK) return true;
  (void)mutex_unlock(NULL, operation);
  return false;
}

/* Played clips arrive in order from the one audio worker. */
static atomic_uint retry_tails, poweroff_tails;
static bool previous_failed, previous_poweroff;

static int log_write(void* user, h2_pal_log_level_t level, const char* scope,
                     const char* message) {
  (void)user;
  (void)level;
  printf("[%s] %s\n", scope, message);
  const char* play = strstr(message, "H202_MFG_PLAY prompt=");
  if (play != NULL) {
    const char* name = play + strlen("H202_MFG_PLAY prompt=");
    if (strcmp(name, "retry") == 0 && previous_failed)
      atomic_fetch_add(&retry_tails, 1u);
    if (strcmp(name, "retry_poweroff") == 0 && previous_poweroff)
      atomic_fetch_add(&poweroff_tails, 1u);
    const size_t length = strlen(name);
    previous_failed = length > 7u &&
                      strcmp(name + length - 7u, "_failed") == 0 &&
                      strcmp(name, "poweroff_failed") != 0;
    previous_poweroff =
        strcmp(name, "poweroff_failed") == 0 || strcmp(name, "unsaved") == 0;
  }
  return H2_PAL_OK;
}
static const h2_pal_log_vtable_t log_vtable = {.write = log_write};
static const h2_pal_log_api_t log_api = {.vtable = &log_vtable};

static h2_pal_result_t button_read(void* user, h2_pal_periph_id_t id,
                                   h2_pal_single_button_reading_t* out) {
  (void)user;
  assert(id >= 1u && id <= 4u);
  out->id = id;
  out->state = atomic_load(&key_mask) & (1u << (id - 1u))
                   ? H2_PAL_BUTTON_STATE_PRESSED
                   : H2_PAL_BUTTON_STATE_RELEASED;
  return H2_PAL_OK;
}
static const h2_pal_button_vtable_t button_vtable = {.read_single_button =
                                                         button_read};
static const h2_pal_button_api_t button_api = {.vtable = &button_vtable};

enum { USB_ID = 5, WHITE_ID = 8, CHRG_ID = 9, DONE_ID = 10, EYES_ID = 7 };

static h2_pal_result_t switch_get(void* user, h2_pal_periph_id_t id,
                                  h2_pal_switch_state_t* out) {
  (void)user;
  bool on = id == USB_ID     ? atomic_load(&usb)
            : id == CHRG_ID  ? atomic_load(&chrg)
            : id == DONE_ID  ? atomic_load(&done_line)
            : id == WHITE_ID ? atomic_load(&white)
                             : false;
  *out = on ? H2_PAL_SWITCH_STATE_ON : H2_PAL_SWITCH_STATE_OFF;
  return H2_PAL_OK;
}
static h2_pal_result_t switch_set(void* user, h2_pal_periph_id_t id,
                                  h2_pal_switch_state_t state) {
  (void)user;
  if (id != WHITE_ID) return H2_PAL_ERR_UNSUPPORTED;
  atomic_store(&white, state == H2_PAL_SWITCH_STATE_ON);
  return H2_PAL_OK;
}
static const h2_pal_switch_vtable_t switch_vtable = {.get = switch_get,
                                                     .set = switch_set};
static const h2_pal_switch_api_t switch_api = {.vtable = &switch_vtable};

static h2_pal_result_t led_frame(void* user, h2_pal_led_id_t id,
                                 const h2_pal_led_color_t* pixels,
                                 size_t count) {
  (void)user;
  assert(id == EYES_ID && count == 6u);
  atomic_fetch_add(&led_frames, 1u);
  atomic_store(&last_color, pixels[3].r << 16 | pixels[3].g << 8 | pixels[3].b);
  return H2_PAL_OK;
}
static const h2_pal_led_vtable_t led_vtable = {.set_frame = led_frame};
static const h2_pal_led_api_t led_api = {.vtable = &led_vtable};

static h2_pal_result_t nfc_scan(void* user, h2_pal_periph_id_t id,
                                h2_pal_nfc_scan_t* out) {
  (void)user;
  memset(out, 0, sizeof(*out));
  out->id = id;
  if (atomic_load(&card)) {
    out->stage = H2_PAL_NFC_STAGE_DISCOVERED;
    out->tag_type = H2_PAL_NFC_TAG_TYPE_ISO14443A;
    out->uid_len = 4u;
    memcpy(out->uid, "\x04\xa1\xb2\xc3", 4);
  }
  return H2_PAL_OK;
}
static const h2_pal_nfc_vtable_t nfc_vtable = {.scan_nfc_reader = nfc_scan};
static const h2_pal_nfc_api_t nfc_api = {.vtable = &nfc_vtable};

/* Associated with the fixture while DHCP never completes. */
static atomic_bool wifi_associated, wifi_online, wifi_wait_locked;
static atomic_uint wifi_status_reads;
static int wifi_status(void* user, h2_pal_wifi_sta_status_t* out) {
  (void)user;
  memset(out, 0, sizeof(*out));
  out->state = H2_PAL_WIFI_STA_STATE_DISCONNECTED;
  if (atomic_load(&wifi_online)) { /* Linked with an address. */
    out->state = H2_PAL_WIFI_STA_STATE_GOT_IP;
    out->ssid_len = strlen("HAIVIVI-MFG");
    memcpy(out->ssid, "HAIVIVI-MFG", out->ssid_len);
    out->ip_valid = 1u;
    out->ip.ip4 = 0x0a000003u;
    return H2_PAL_OK;
  }
  if (atomic_load(&wifi_associated)) {
    out->state = H2_PAL_WIFI_STA_STATE_CONNECTED;
    out->ssid_len = strlen("HAIVIVI-MFG");
    memcpy(out->ssid, "HAIVIVI-MFG", out->ssid_len);
    atomic_fetch_add(&wifi_status_reads, 1u);
    if (operation_held()) atomic_store(&wifi_wait_locked, true);
  }
  return H2_PAL_OK;
}
static int wifi_scan(void* user, const h2_pal_wifi_scan_request_t* request,
                     h2_pal_wifi_scan_result_fn on_result, void* cb_user,
                     uint32_t timeout_ms) {
  (void)user;
  (void)request;
  (void)on_result;
  (void)cb_user;
  (void)timeout_ms;
  return H2_PAL_OK;
}
static int wifi_connect(void* user, const h2_pal_wifi_sta_config_t* config,
                        uint32_t timeout_ms) {
  (void)user;
  (void)timeout_ms;
  assert(strcmp(config->ssid, "HAIVIVI-MFG") == 0);
  atomic_store(&wifi_connect_locked, operation_held());
  atomic_fetch_add(&wifi_connects, 1u);
  if (atomic_load(&wifi_associated)) return H2_PAL_OK;
  return H2_PAL_ERR_UNAVAILABLE; /* No fixture AP on the host. */
}
static int wifi_disconnect(void* user) {
  (void)user;
  return H2_PAL_OK;
}
static const h2_pal_wifi_sta_vtable_t wifi_vtable = {
    .get_status = wifi_status,
    .scan = wifi_scan,
    .connect = wifi_connect,
    .disconnect = wifi_disconnect};
static const h2_pal_wifi_sta_api_t wifi_api = {.vtable = &wifi_vtable};

static h2_pal_result_t power_shutdown(void* user, uint32_t reason) {
  (void)user;
  (void)reason;
  atomic_store(&shutdown_key_down, (atomic_load(&key_mask) & 1u) != 0u);
  atomic_store(&shutdown_locked, operation_held());
  atomic_fetch_add(&shutdowns, 1u);
  return H2_PAL_ERR_IO; /* Host cannot power off: POWEROFF_FAILED path. */
}
static h2_pal_result_t power_reboot(void* user, uint32_t reason) {
  (void)user;
  (void)reason;
  atomic_store(&reboot_locked, operation_held());
  atomic_fetch_add(&reboots, 1u);
  return H2_PAL_ERR_IO; /* Host cannot reboot: RESET_FAILED path. */
}
static const h2_pal_power_vtable_t power_vtable = {.shutdown = power_shutdown,
                                                   .reboot = power_reboot};
static const h2_pal_power_api_t power_api = {.vtable = &power_vtable};

/* Audio fake enforcing the PAL observer contract: registration only while
 * microphone and speaker are stopped, per-lane capture frames. */
static struct {
  bool mic, speaker;
  bool observer_set;
  h2_audio_aec_observer_t observer;
  bool track_open;
  uint64_t t;
  int16_t raw[960], ref[320], processed[2][320];
  unsigned writes;
} audio;
static h2_pal_audio_track_t audio_track;

static int a_start_mic(void* user) {
  (void)user;
  assert(!audio.mic);
  audio.mic = true;
  atomic_fetch_add(&mic_starts, 1u);
  return H2_PAL_OK;
}
static int a_stop_mic(void* user) {
  (void)user;
  audio.mic = false;
  return H2_PAL_OK;
}
static int a_start_speaker(void* user) {
  (void)user;
  audio.speaker = true;
  return H2_PAL_OK;
}
static int a_stop_speaker(void* user) {
  (void)user;
  if (audio.track_open) return H2_PAL_ERR_BUSY;
  audio.speaker = false;
  return H2_PAL_OK;
}
static int a_observer(void* user, const h2_audio_aec_observer_t* observer) {
  (void)user;
  if (audio.mic || audio.speaker) return H2_PAL_ERR_INVALID_STATE;
  audio.observer_set = observer != NULL;
  if (observer != NULL) audio.observer = *observer;
  return H2_PAL_OK;
}
static int a_volume(void* user, uint32_t percent) {
  (void)user;
  assert(percent <= 100u);
  atomic_store(&speaker_volume, percent);
  return H2_PAL_OK;
}
static int a_mic_read(void* user, h2_audio_frame_t* out, uint32_t timeout_ms) {
  (void)user;
  (void)timeout_ms;
  if (!audio.mic) return H2_PAL_ERR_INVALID_STATE;
  sleep_ms(1);
  int16_t* stereo = out->data;
  bool silent = atomic_load(&silent_right);
  for (unsigned i = 0; i < 320u; ++i, ++audio.t) {
    float tone = sinf(2.0f * 3.14159265f * (float)audio.t / 16.0f);
    audio.raw[i * 3u] = pcm(0.3f * tone);
    audio.raw[i * 3u + 1u] = silent ? 0 : pcm(0.3f * tone);
    audio.raw[i * 3u + 2u] = audio.ref[i] = pcm(0.5f * tone);
    for (unsigned lane = 0; lane < 2u; ++lane)
      stereo[i * 2u + lane] = audio.processed[lane][i] =
          lane == 1u && silent ? 0 : pcm(0.01f * tone);
  }
  if (audio.observer_set) {
    for (uint8_t lane = 0; lane < 2u; ++lane) {
      h2_audio_frame_t raw = {.data = audio.raw,
                              .bytes = sizeof(audio.raw),
                              .capacity = sizeof(audio.raw),
                              .sample_rate_hz = 16000u,
                              .samples_per_channel = 320u,
                              .channels = 3u,
                              .sample_format = H2_AUDIO_SAMPLE_S16LE};
      h2_audio_frame_t ref = {.data = audio.ref,
                              .bytes = 640u,
                              .capacity = 640u,
                              .sample_rate_hz = 16000u,
                              .samples_per_channel = 320u,
                              .channels = 1u,
                              .sample_format = H2_AUDIO_SAMPLE_S16LE};
      h2_audio_frame_t proc = ref;
      proc.data = audio.processed[lane];
      const h2_audio_aec_frame_t frame = {.raw = &raw,
                                          .reference_input = &ref,
                                          .processed = &proc,
                                          .microphone_lane = lane,
                                          .reference_lane = 2u,
                                          .microphone_mask = 3u};
      audio.observer.on_capture(audio.observer.user, &frame);
    }
  }
  out->bytes = 1280u;
  out->channels = 2u;
  out->samples_per_channel = 320u;
  out->sample_rate_hz = 16000u;
  out->sample_format = H2_AUDIO_SAMPLE_S16LE;
  return H2_PAL_OK;
}
static int a_write(h2_pal_audio_track_t* track, const h2_audio_frame_t* frame,
                   uint32_t timeout_ms) {
  (void)track;
  (void)timeout_ms;
  assert(audio.speaker && frame->bytes == 640u && frame->channels == 1u);
  const int16_t* pcm16 = frame->data;
  for (unsigned i = 0; i < 320u; ++i)
    assert(pcm16[i] < 32767 && pcm16[i] > -32768); /* Never clipped. */
  ++audio.writes;
  if (!audio.mic) sleep_ms(1);
  return H2_PAL_OK;
}
static int a_close(h2_pal_audio_track_t* track) {
  (void)track;
  audio.track_open = false;
  return H2_PAL_OK;
}
static int a_drain(h2_pal_audio_track_t* track, uint32_t timeout_ms) {
  (void)track;
  (void)timeout_ms;
  return H2_PAL_OK;
}
static int a_track(void* user, const h2_audio_track_config_t* config,
                   h2_pal_audio_track_t** out) {
  (void)user;
  assert(config->format.sample_rate_hz == 16000u);
  assert(config->format.frame_samples_per_channel == 320u);
  assert(!audio.track_open);
  audio.track_open = true;
  audio_track = (h2_pal_audio_track_t){
      .write = a_write, .close = a_close, .drain = a_drain};
  *out = &audio_track;
  return H2_PAL_OK;
}
static const h2_pal_audio_vtable_t audio_vtable = {
    .start_mic = a_start_mic,
    .stop_mic = a_stop_mic,
    .start_speaker = a_start_speaker,
    .stop_speaker = a_stop_speaker,
    .mic_read = a_mic_read,
    .create_track = a_track,
    .set_speaker_volume_percent = a_volume,
    .set_aec_observer = a_observer};
static const h2_pal_audio_api_t audio_api = {.vtable = &audio_vtable};

/* In-memory "h2loader" preference namespace for h2_loader_mfg_*. */
typedef struct entry {
  char key[32];
  uint8_t blob[64];
  size_t len;
  bool is_u32;
  uint32_t u32;
  bool used;
} entry_t;
static entry_t store[8];
static atomic_int fail_set_blob;
static h2_pal_pref_namespace_t ns;

static entry_t* find(const char* key, bool create) {
  for (unsigned i = 0; i < 8; ++i)
    if (store[i].used && strcmp(store[i].key, key) == 0) return &store[i];
  if (!create) return NULL;
  for (unsigned i = 0; i < 8; ++i)
    if (!store[i].used) {
      memset(&store[i], 0, sizeof(store[i]));
      store[i].used = true;
      snprintf(store[i].key, sizeof(store[i].key), "%s", key);
      return &store[i];
    }
  abort();
}
static int ns_ok(h2_pal_pref_namespace_t* n) {
  (void)n;
  return H2_PAL_OK;
}
static int ns_get_blob(h2_pal_pref_namespace_t* n, const h2_pal_mem_api_t* m,
                       const char* key, void** out, size_t* len) {
  (void)n;
  entry_t* e = find(key, false);
  if (e == NULL || e->is_u32) return H2_PAL_ERR_NOT_FOUND;
  *out = h2_pal_mem_alloc(m, e->len);
  memcpy(*out, e->blob, e->len);
  *len = e->len;
  return H2_PAL_OK;
}
static int ns_set_blob(h2_pal_pref_namespace_t* n, const char* key,
                       const void* data, size_t len) {
  (void)n;
  if (atomic_load(&fail_set_blob)) return H2_PAL_ERR_IO;
  entry_t* e = find(key, true);
  e->is_u32 = false;
  memcpy(e->blob, data, len);
  e->len = len;
  return H2_PAL_OK;
}
static int ns_get_u32(h2_pal_pref_namespace_t* n, const char* key,
                      uint32_t* out) {
  (void)n;
  entry_t* e = find(key, false);
  if (e == NULL || !e->is_u32) return H2_PAL_ERR_NOT_FOUND;
  *out = e->u32;
  return H2_PAL_OK;
}
static int ns_set_u32(h2_pal_pref_namespace_t* n, const char* key,
                      uint32_t value) {
  (void)n;
  entry_t* e = find(key, true);
  e->is_u32 = true;
  e->u32 = value;
  return H2_PAL_OK;
}
static int pref_open(void* user, const char* space,
                     h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t** out) {
  (void)user;
  (void)space;
  (void)mode;
  ns = (h2_pal_pref_namespace_t){.close = ns_ok,
                                 .get_blob = ns_get_blob,
                                 .set_blob = ns_set_blob,
                                 .get_u32 = ns_get_u32,
                                 .set_u32 = ns_set_u32,
                                 .commit = ns_ok};
  *out = &ns;
  return H2_PAL_OK;
}
static const h2_pal_pref_vtable_t pref_vtable = {.open = pref_open};
static const h2_pal_pref_api_t pref_api = {.vtable = &pref_vtable};

/* 200 silent samples, as before ADPCM. */
static const unsigned char prompt_adpcm[100] = {0};
static const size_t prompt_size = sizeof(prompt_adpcm);

static h2_h202_mfg_profile_t profile = {
    .controller = {.charger_timeout_ms = 5000u,
                   .nfc_timeout_ms = 5000u,
                   .wifi_timeout_ms = 3000u,
                   .power_hold_ms = 400u},
    .keys = {1, 2, 3, 4},
    .usb_insert = USB_ID,
    .charging = CHRG_ID,
    .charge_done = DONE_ID,
    .white = WHITE_ID,
    .nfc = 6,
    .eyes = EYES_ID,
    .eye_count = 6,
    .left_eye_mask = 0x15,
    .colors = {[H2_H202_MFG_COLOR_BLUE] = {0, 0, 64, 0},
               [H2_H202_MFG_COLOR_BLUE_LEFT] = {0, 0, 64, 0},
               [H2_H202_MFG_COLOR_BLUE_RIGHT] = {0, 0, 64, 0},
               [H2_H202_MFG_COLOR_GREEN] = {0, 64, 0, 0},
               [H2_H202_MFG_COLOR_YELLOW] = {64, 48, 0, 0},
               [H2_H202_MFG_COLOR_RED] = {64, 0, 0, 0},
               [H2_H202_MFG_COLOR_WHITE] = {32, 32, 32, 0}},
    .tick_ms = 10u,
    .key_debounce_ms = 20u,
    .charger_debounce_ms = 50u,
    .charge_window_ms = 2000u,
    .charge_toggles = 2u,
    .charge_blink_period_ms = 500u,
    .sample_rate_hz = 16000u,
    .frame_samples = 320u,
    .volume_percent = 35u,
    .aging_scale = {.sample_rate_hz = 16000u,
                    .notes_hz = {523u, 659u, 784u, 659u, 587u, 698u, 880u,
                                 698u},
                    .note_ms = 30u,
                    .gap_ms = 0u,
                    .amplitude = 10922},
    .monitor_gain_q15 = 8192u,
    .aec = {.sample_rate_hz = 16000u,
            .convergence_ms = 200u,
            .measure_ms = 600u,
            .block_ms = 100u},
    .aging_frame_ms = 40u,
    .wifi_ssid = "HAIVIVI-MFG",
    .wifi_password = "!haivivi",
    .ntp_host = "time.apple.com",
};

static h2_h202_mfg_service_t* service;

static h2_h202_mfg_status_t status(void) {
  h2_h202_mfg_status_t s;
  assert(h2_h202_mfg_service_status(service, &s) == H2_PAL_OK);
  return s;
}

static void wait_for(h2_h202_mfg_item_t item, h2_h202_mfg_phase_t phase) {
  for (unsigned i = 0; i < 1000u; ++i) {
    h2_h202_mfg_status_t s = status();
    if (s.item == item && s.phase == phase) return;
    sleep_ms(10);
  }
  h2_h202_mfg_status_t s = status();
  fprintf(stderr, "timeout waiting item=%d phase=%d (item=%d phase=%d)\n", item,
          phase, s.item, s.phase);
  abort();
}

static void keys_now(unsigned mask) {
  atomic_store(&key_mask, mask);
  sleep_ms(60);
}

static void click(unsigned key) {
  keys_now(atomic_load(&key_mask) | key);
  keys_now(atomic_load(&key_mask) & ~key);
}

static void start(void) {
  memset(&audio, 0, sizeof(audio));
  const h2_h202_mfg_service_config_t config = {.runtime = &runtime,
                                               .profile = &profile,
                                               .operation_sync = &sync_api,
                                               .operation_mutex = operation};
  assert(h2_h202_mfg_service_start(&config, &service) == H2_PAL_OK);
}

static void stop(void) {
  assert(h2_h202_mfg_service_stop(&service) == H2_PAL_OK && service == NULL);
  assert(atomic_load(&tasks) == 0);
  assert(!audio.mic && !audio.speaker && !audio.observer_set);
}

static void stored(h2_loader_mfg_summary_t* out) {
  int present = 0;
  assert(h2_loader_mfg_read(&pref_api, &mem, out, &present) == H2_PAL_OK);
  assert(present && out->total == H2_H202_MFG_ITEM_COUNT);
}

static void test_session(void) {
  start();
  wait_for(H2_H202_MFG_CHARGER, H2_H202_MFG_PHASE_ACTIVE);
  /* Unplugged at start: insert, then remove. */
  atomic_store(&usb, true);
  sleep_ms(200);
  atomic_store(&usb, false);
  wait_for(H2_H202_MFG_NFC, H2_H202_MFG_PHASE_ACTIVE);
  atomic_store(&card, true);
  /* No Wi-Fi fixture: the background check failed under the lock. */
  wait_for(H2_H202_MFG_WIFI, H2_H202_MFG_PHASE_FAILED);
  assert(atomic_load(&wifi_connect_locked));
  /* Left passes, the silent right lane fails, then retries after repair. */
  atomic_store(&silent_right, true);
  keys_now(2u);
  keys_now(2u | 4u);
  keys_now(2u);
  keys_now(0u);
  wait_for(H2_H202_MFG_RIGHT_MIC, H2_H202_MFG_PHASE_FAILED);
  assert(status().results[H2_H202_MFG_LEFT_MIC] == H2_H202_MFG_PASSED);
  atomic_store(&silent_right, false);
  click(2u);
  wait_for(H2_H202_MFG_LISTEN, H2_H202_MFG_PHASE_ACTIVE);
  click(2u);
  wait_for(H2_H202_MFG_POWER, H2_H202_MFG_PHASE_ACTIVE);
  click(1u);
  wait_for(H2_H202_MFG_PLUS, H2_H202_MFG_PHASE_ACTIVE);
  click(4u);
  wait_for(H2_H202_MFG_MINUS, H2_H202_MFG_PHASE_ACTIVE);
  click(8u);
  /* Wi-Fi was skipped: no aging, yellow. */
  wait_for(H2_H202_MFG_ITEM_COUNT, H2_H202_MFG_PHASE_WAITING);
  assert(status().color == H2_H202_MFG_COLOR_YELLOW);
  assert(atomic_load(&last_color) == (64 << 16 | 48 << 8));
  assert(status().results[H2_H202_MFG_WIFI] == H2_H202_MFG_SKIPPED);
  assert(status().results[H2_H202_MFG_AGING] == H2_H202_MFG_UNTESTED);
  /* Charge indicator runs in the background from the raw lines. */
  atomic_store(&usb, true);
  atomic_store(&chrg, true);
  sleep_ms(200);
  assert(atomic_load(&white));
  atomic_store(&usb, false);
  sleep_ms(200);
  assert(!atomic_load(&white));
  atomic_store(&chrg, false);
  /* 2 s (here 400 ms) hold: commit and power off without waiting for release.
   */
  keys_now(1u);
  wait_for(H2_H202_MFG_ITEM_COUNT, H2_H202_MFG_PHASE_POWEROFF_FAILED);
  assert(status().committed && atomic_load(&shutdowns) == 1u);
  assert(atomic_load(&key_mask) == 1u);
  assert(atomic_load(&shutdown_key_down) && atomic_load(&shutdown_locked));
  keys_now(0u);
  h2_loader_mfg_summary_t summary;
  stored(&summary);
  for (unsigned i = 0; i < H2_H202_MFG_ITEM_COUNT; ++i)
    assert(summary.step_status[i] == status().results[i]);
  /* Failure prompts end with the shared retry instruction. */
  for (unsigned i = 0; i < 200u && atomic_load(&poweroff_tails) == 0u; ++i)
    sleep_ms(10);
  assert(atomic_load(&retry_tails) > 0u && atomic_load(&poweroff_tails) > 0u);
  stop();
  puts("service session, network ownership and shutdown ok");
}

static void test_wifi_waits_for_address(void) {
  /* BK connect returns once associated, before DHCP. Waiting for the address
   * uses the whole Wi-Fi budget (3 s here) instead of failing at once, and
   * does not hold the operation mutex that App confirm also needs. */
  atomic_store(&wifi_associated, true);
  uint64_t begin = 0u, end = 0u;
  assert(now_ms(NULL, &begin) == H2_PAL_OK);
  start();
  for (unsigned i = 0; i < 600u && status().wifi != H2_H202_MFG_NET_FAILED; ++i)
    sleep_ms(10);
  assert(now_ms(NULL, &end) == H2_PAL_OK);
  assert(status().wifi == H2_H202_MFG_NET_FAILED);
  assert(end - begin >= 2500u);
  assert(atomic_load(&wifi_status_reads) > 1u);
  assert(!atomic_load(&wifi_wait_locked));
  stop();
  atomic_store(&wifi_associated, false);
  puts("wifi waits for a DHCP address within its budget ok");
}

static void test_reset(void) {
  uint8_t results[H2_H202_MFG_ITEM_COUNT];
  memset(results, H2_H202_MFG_PASSED, sizeof(results));
  results[H2_H202_MFG_AGING] = H2_H202_MFG_FAILED;
  start();
  wait_for(H2_H202_MFG_CHARGER, H2_H202_MFG_PHASE_ACTIVE);
  /* A record saved by an earlier shutdown, then Record + short power. */
  assert(h2_h202_mfg_progress_commit(&pref_api, results) == H2_PAL_OK);
  keys_now(2u);
  keys_now(2u | 1u);
  keys_now(2u);
  keys_now(0u);
  wait_for(H2_H202_MFG_CHARGER, H2_H202_MFG_PHASE_RESET_FAILED);
  assert(atomic_load(&reboots) == 1u && atomic_load(&reboot_locked));
  h2_loader_mfg_summary_t summary;
  stored(&summary);
  for (unsigned i = 0; i < H2_H202_MFG_ITEM_COUNT; ++i)
    assert(summary.step_status[i] == H2_LOADER_MFG_STEP_UNTESTED);
  stop();
  puts("service reset gesture ok");
}

/* Before management: white eyes, then the boot prompt played to the end
 * with the speaker released again. A missing prompt still lights the eyes. */
static void test_boot_feedback(void) {
  memset(&audio, 0, sizeof(audio));
  unsigned frames = atomic_load(&led_frames);
  assert(h2_h202_mfg_boot_feedback(&runtime, &profile) == H2_PAL_OK);
  assert(atomic_load(&led_frames) == frames + 1u);
  assert(atomic_load(&last_color) == (32 << 16 | 32 << 8 | 32));
  assert(audio.writes > 0u && atomic_load(&speaker_volume) == 35u);
  assert(!audio.speaker && !audio.track_open && !audio.mic);
  const h2_h202_mfg_adpcm_t saved = profile.prompts[H2_H202_MFG_PROMPT_BOOT];
  profile.prompts[H2_H202_MFG_PROMPT_BOOT] = (h2_h202_mfg_adpcm_t){0};
  unsigned writes = audio.writes;
  assert(h2_h202_mfg_boot_feedback(&runtime, &profile) == H2_PAL_ERR_NOT_FOUND);
  assert(atomic_load(&led_frames) == frames + 2u && audio.writes == writes);
  assert(!audio.speaker && !audio.track_open);
  profile.prompts[H2_H202_MFG_PROMPT_BOOT] = saved;
  assert(h2_h202_mfg_boot_feedback(NULL, &profile) == H2_PAL_ERR_INVALID_ARG);
  puts("boot feedback ok");
}

static void test_resume_and_aging(void) {
  /* Resume at the first item not PASSED; the record is cleared at boot. */
  start();
  for (unsigned i = 0; i < 100u && status().item != H2_H202_MFG_WIFI; ++i)
    sleep_ms(10);
  assert(status().item == H2_H202_MFG_WIFI);
  h2_loader_mfg_summary_t summary;
  stored(&summary);
  for (unsigned i = 0; i < H2_H202_MFG_ITEM_COUNT; ++i)
    assert(summary.step_status[i] == H2_LOADER_MFG_STEP_UNTESTED);
  stop();
  /* Nine PASSED results go straight to aging. */
  uint8_t results[H2_H202_MFG_ITEM_COUNT];
  memset(results, H2_H202_MFG_PASSED, sizeof(results));
  results[H2_H202_MFG_AGING] = H2_H202_MFG_FAILED;
  assert(h2_h202_mfg_progress_commit(&pref_api, results) == H2_PAL_OK);
  unsigned mics = atomic_load(&mic_starts);
  /* Wi-Fi is still linked from stage 04: aging keeps it, no reconnect. */
  atomic_store(&wifi_online, true);
  unsigned connects = atomic_load(&wifi_connects);
  start();
  wait_for(H2_H202_MFG_AGING, H2_H202_MFG_PHASE_ACTIVE);
  unsigned frames = atomic_load(&led_frames);
  sleep_ms(400);
  assert(atomic_load(&led_frames) > frames + 3u); /* Rainbow running. */
  assert(atomic_load(&wifi_connects) == connects);
  /* A dropped link is reconnected (and fails on the host fixture). */
  atomic_store(&wifi_online, false);
  for (unsigned i = 0; i < 300u && atomic_load(&wifi_connects) == connects; ++i)
    sleep_ms(10);
  assert(atomic_load(&wifi_connects) > connects);
  /* Playback only, like H106: no microphone or AEC during aging. */
  assert(atomic_load(&mic_starts) == mics && !audio.observer_set);
  assert(atomic_load(&speaker_volume) == 35u);
  click(H2_H202_MFG_KEY_PLUS);
  assert(atomic_load(&speaker_volume) == 55u);
  click(H2_H202_MFG_KEY_MINUS);
  assert(atomic_load(&speaker_volume) == 35u);
  /* Repeated presses clamp at both ends instead of wrapping. */
  for (unsigned i = 0; i < 5u; ++i) click(H2_H202_MFG_KEY_PLUS);
  assert(atomic_load(&speaker_volume) == 100u);
  for (unsigned i = 0; i < 6u; ++i) click(H2_H202_MFG_KEY_MINUS);
  assert(atomic_load(&speaker_volume) == 0u);
  assert(status().phase == H2_H202_MFG_PHASE_ACTIVE);
  click(2u);
  wait_for(H2_H202_MFG_ITEM_COUNT, H2_H202_MFG_PHASE_WAITING);
  assert(status().results[H2_H202_MFG_AGING] == H2_H202_MFG_PASSED);
  assert(status().color == H2_H202_MFG_COLOR_GREEN);
  sleep_ms(100); /* No late rainbow frame overwrites green. */
  assert(atomic_load(&last_color) == (64 << 8));
  assert(atomic_load(&speaker_volume) == 35u); /* Prompt volume is restored. */
  /* The pass is saved without powering off, for the Loader MFG gate. */
  bool accepted = false;
  for (unsigned i = 0; i < 100u && !accepted; ++i) {
    stored(&summary);
    accepted = summary.step_status[H2_H202_MFG_AGING] == H2_H202_MFG_PASSED;
    if (!accepted) sleep_ms(10);
  }
  for (unsigned i = 0; i < H2_H202_MFG_ITEM_COUNT; ++i)
    assert(summary.step_status[i] == H2_H202_MFG_PASSED);
  /* A failed shutdown commit still announces unsaved and keeps running. */
  atomic_store(&fail_set_blob, 1);
  keys_now(1u);
  sleep_ms(500);
  keys_now(0u);
  wait_for(H2_H202_MFG_ITEM_COUNT, H2_H202_MFG_PHASE_UNSAVED);
  assert(!status().committed);
  atomic_store(&fail_set_blob, 0);
  stop();
  /* A management reboot (stop without hold) keeps the saved pass: boot
   * clears it, stage 10 saves it again. */
  start();
  wait_for(H2_H202_MFG_ITEM_COUNT, H2_H202_MFG_PHASE_WAITING);
  accepted = false;
  for (unsigned i = 0; i < 100u && !accepted; ++i) {
    stored(&summary);
    accepted = summary.step_status[H2_H202_MFG_AGING] == H2_H202_MFG_PASSED;
    if (!accepted) sleep_ms(10);
  }
  assert(accepted);
  stop();
  assert(h2_h202_mfg_progress_clear(&pref_api) == H2_PAL_OK);
  puts("service resume, aging, accepted pass and unsaved shutdown ok");
}

int main(void) {
  test_measurement();
  test_scale();
  test_prompt_playback();
  for (unsigned i = 0; i < H2_H202_MFG_PROMPT_COUNT; ++i)
    profile.prompts[i] = (h2_h202_mfg_adpcm_t){prompt_adpcm, &prompt_size};
  runtime = (h2_runtime_t){.board = "h202",
                           .mem = &mem,
                           .log = &log_api,
                           .time = &time_api,
                           .task = &task_api,
                           .sync = &sync_api,
                           .pref = &pref_api,
                           .button = &button_api,
                           .switch_api = &switch_api,
                           .led = &led_api,
                           .nfc = &nfc_api,
                           .wifi_sta = &wifi_api,
                           .power = &power_api,
                           .audio = &audio_api};
  assert(mutex_create(NULL, NULL, &operation) == H2_PAL_OK);
  test_boot_feedback();
  test_wifi_waits_for_address();
  test_session();
  test_resume_and_aging();
  test_reset();
  assert(mutex_destroy(NULL, operation) == H2_PAL_OK);
  return 0;
}
