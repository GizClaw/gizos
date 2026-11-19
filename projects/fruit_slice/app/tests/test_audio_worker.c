/* Real task/queue/Runtime boundaries and an independently clocked PCM sink.
 * This measures digital supply only, never physical DAC/VFS or speaker output.
 */
#include "h2_fruit_slice.h"
#include "h2_fruit_slice_audio.h"
#include "h2_fruit_slice_audio_worker.h"
#include "h2_fruit_slice_game.h"
#include "h2_desktop_platform.h"
#include "h2_pal_unsupported.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static uint64_t now(void) {
  uint64_t value = 0;
  assert(h2_pal_time_get_monotonic_ms(h2_desktop_platform_time_api(), &value) ==
         0);
  return value;
}
static void delay(uint32_t ms) {
  assert(h2_pal_time_sleep_ms(h2_desktop_platform_time_api(), ms) == 0);
}

typedef struct sink {
  pthread_mutex_t lock;
  h2_pal_audio_track_t track;
  uint64_t until, last, missing, max_gap, first_nonzero;
  unsigned writes, opens, closes, stops, blocks, fail_write;
  int fail_create, fail_close;
  int16_t first_pcm[640];
} sink_t;
static sink_t sink = {.lock = PTHREAD_MUTEX_INITIALIZER};
static int info(void* user, h2_audio_info_t* out) {
  (void)user;
  *out = (h2_audio_info_t){
      .available = 1,
      .playback_supported = 1,
      .playback_format = {16000, 320, 1, H2_AUDIO_SAMPLE_S16LE}};
  return 0;
}
static int start(void* user) {
  (void)user;
  return 0;
}
static int stop(void* user) {
  sink_t* s = user;
  pthread_mutex_lock(&s->lock);
  ++s->stops;
  pthread_mutex_unlock(&s->lock);
  return 0;
}
static int write_pcm(h2_pal_audio_track_t* track, const h2_audio_frame_t* frame,
                     uint32_t timeout) {
  sink_t* s = track->user;
  assert(timeout == 0 && frame->bytes == 640);
  pthread_mutex_lock(&s->lock);
  if (s->fail_write && s->writes >= s->fail_write) {
    pthread_mutex_unlock(&s->lock);
    return H2_PAL_ERR_IO;
  }
  if (s->blocks) {
    --s->blocks;
    pthread_mutex_unlock(&s->lock);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  const uint64_t t = now();
  if (s->writes) {
    if (t > s->until) s->missing += t - s->until;
    if (t - s->last > s->max_gap) s->max_gap = t - s->last;
  }
  s->last = t;
  if (s->until < t) s->until = t;
  if (s->until - t > 20u) {
    pthread_mutex_unlock(&s->lock);
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  if (!s->first_nonzero) {
    const int16_t* samples = frame->data;
    for (unsigned i = 0; i < 320; ++i)
      if (samples[i]) {
        s->first_nonzero = s->until + i / 16u;
        break;
      }
  }
  if (s->writes < 2u)
    memcpy(s->first_pcm + s->writes * 320u, frame->data, 640u);
  s->until += 20u;
  ++s->writes;
  pthread_mutex_unlock(&s->lock);
  return 0;
}
static int close_pcm(h2_pal_audio_track_t* track) {
  sink_t* s = track->user;
  pthread_mutex_lock(&s->lock);
  ++s->closes;
  const int rc = s->fail_close;
  pthread_mutex_unlock(&s->lock);
  return rc;
}
static int create(void* user, const h2_audio_track_config_t* cfg,
                  h2_pal_audio_track_t** out) {
  sink_t* s = user;
  assert(cfg->buffer_frames == 2u);
  pthread_mutex_lock(&s->lock);
  if (s->fail_create) {
    const int rc = s->fail_create;
    pthread_mutex_unlock(&s->lock);
    return rc;
  }
  ++s->opens;
  s->until = 0;
  s->last = 0;
  s->track =
      (h2_pal_audio_track_t){.user = s, .write = write_pcm, .close = close_pcm};
  *out = &s->track;
  pthread_mutex_unlock(&s->lock);
  return 0;
}
static const h2_pal_audio_vtable_t audio_vt = {.get_info = info,
                                               .start_speaker = start,
                                               .stop_speaker = stop,
                                               .create_track = create};
static const h2_pal_audio_api_t audio_api = {.user = &sink,
                                             .vtable = &audio_vt};
static void reset_sink(void) {
  /* Every preceding worker has joined before fixture reuse. */
  memset((char*)&sink + sizeof(sink.lock), 0, sizeof(sink) - sizeof(sink.lock));
}

static int join_failures, start_failures, queue_create_failures,
    queue_close_failures;
static unsigned live_queues;
static int queue_create(void* u, const h2_pal_queue_config_t* c,
                        h2_pal_queue_t** q) {
  (void)u;
  if (queue_create_failures && --queue_create_failures == 0)
    return H2_PAL_ERR_NO_MEMORY;
  int rc = h2_pal_queue_create(h2_desktop_platform_queue_api(), c, q);
  if (!rc) ++live_queues;
  return rc;
}
static void queue_destroy(void* u, h2_pal_queue_t* q) {
  (void)u;
  if (q) {
    assert(live_queues);
    --live_queues;
  }
  h2_pal_queue_destroy(h2_desktop_platform_queue_api(), q);
}
static int queue_close(void* u, h2_pal_queue_t* q) {
  (void)u;
  if (queue_close_failures) {
    --queue_close_failures;
    return H2_PAL_ERR_IO;
  }
  return h2_pal_queue_close(h2_desktop_platform_queue_api(), q);
}
static h2_pal_queue_vtable_t queue_vt;
static h2_pal_queue_api_t queue_api;
static void init_queue_api(void) {
  queue_vt = *h2_desktop_platform_queue_api()->vtable;
  queue_vt.create = queue_create;
  queue_vt.destroy = queue_destroy;
  queue_vt.close = queue_close;
  queue_api =
      (h2_pal_queue_api_t){h2_desktop_platform_queue_api()->user, &queue_vt};
}
static int task_start(void* u, const h2_pal_task_options_t* opt,
                      h2_pal_task_entry_t entry, void* ctx,
                      h2_pal_task_t** out) {
  (void)u;
  if (start_failures) {
    --start_failures;
    return H2_PAL_ERR_NO_MEMORY;
  }
  return h2_pal_task_start(h2_desktop_platform_task_api(), opt, entry, ctx,
                           out);
}
static int task_join(void* u, h2_pal_task_t* task) {
  (void)u;
  if (join_failures) {
    --join_failures;
    return H2_PAL_ERR_BUSY;
  }
  return h2_pal_task_join(h2_desktop_platform_task_api(), task);
}
static const h2_pal_task_vtable_t task_vt = {.start = task_start,
                                             .join = task_join};
static const h2_pal_task_api_t task_api = {.vtable = &task_vt};
static h2_runtime_t direct_runtime(void) {
  return (h2_runtime_t){.audio = &audio_api,
                        .mem = h2_desktop_platform_default_allocator(),
                        .pref = h2_pal_unsupported_pref_api(),
                        .time = h2_desktop_platform_time_api(),
                        .queue = &queue_api,
                        .task = &task_api};
}
static void test_owner_lifecycle_and_latency(void) {
  reset_sink();
  h2_runtime_t rt = direct_runtime();
  h2_fruit_slice_audio_t a;
  assert(h2_fruit_slice_audio_init(&a, &rt) == 0);
  a.mute = H2_FRUIT_SLICE_MUTE_MUSIC;
  a.worker_requested = true;
  assert(h2_fruit_slice_audio_prepare(&a) == 0);
  for (unsigned failure = 1; failure <= 2; ++failure) {
    queue_create_failures = (int)failure;
    assert(h2_fruit_slice_audio_worker_start(&a) == H2_PAL_ERR_NO_MEMORY);
    assert(live_queues == 0u && a.pcm && a.track);
  }
  start_failures = 1;
  assert(h2_fruit_slice_audio_worker_start(&a) == H2_PAL_ERR_NO_MEMORY);
  assert(a.pcm && a.track && !a.worker);
  assert(h2_fruit_slice_audio_worker_start(&a) == 0);
  delay(30);
  assert(sink.writes == 0); /* Prepared worker stays silent before gesture. */
  assert(h2_fruit_slice_audio_pump(&a, now()) == 0);
  delay(60);
  uint64_t emitted = now();
  h2_fruit_slice_audio_emit(&a, H2_FRUIT_SLICE_SOUND_SLICE, emitted);
  delay(180); /* Main/render thread is unavailable through the whole onset. */
  pthread_mutex_lock(&sink.lock);
  const uint64_t onset = sink.first_nonzero;
  pthread_mutex_unlock(&sink.lock);
  assert(onset >= emitted && onset - emitted <= 80u);
  printf("worker digital SFX onset=%llu ms\n",
         (unsigned long long)(onset - emitted));
  queue_close_failures = 1;
  assert(h2_fruit_slice_audio_suspend(&a) == H2_PAL_ERR_IO);
  assert(a.worker && !a.pcm && live_queues == 2u);
  join_failures = 1;
  assert(h2_fruit_slice_audio_suspend(&a) == H2_PAL_ERR_BUSY);
  assert(a.worker && !a.pcm && !a.track && sink.closes == 0);
  assert(h2_fruit_slice_audio_suspend(&a) == 0);
  assert(!a.worker && a.pcm && sink.closes == 1);
  const unsigned written = sink.writes;
  delay(30);
  assert(sink.writes == written);
  assert(h2_fruit_slice_audio_pump(&a, now()) == 0);
  delay(35);
  assert(a.worker);
  assert(h2_fruit_slice_audio_set_mute(&a, 3u) == H2_PAL_ERR_UNSUPPORTED);
  assert(a.mute == 3u && h2_fruit_slice_audio_pump(&a, now()) == 0 &&
         !a.worker);
  assert(h2_fruit_slice_audio_close(&a) == 0);
  assert(sink.opens == sink.closes && live_queues == 0u);
}
static void test_pcm_identity(void) {
  for (uint8_t mute = 0u; mute < 3u; ++mute) {
    int16_t expected[640];
    for (unsigned worker = 0; worker < 2u; ++worker) {
      reset_sink();
      h2_runtime_t rt = direct_runtime();
      h2_fruit_slice_audio_t a;
      assert(h2_fruit_slice_audio_init(&a, &rt) == 0);
      a.mute = mute;
      h2_fruit_slice_audio_emit(&a, H2_FRUIT_SLICE_SOUND_SLICE, 1000u);
      h2_fruit_slice_audio_emit(&a, H2_FRUIT_SLICE_SOUND_CARD, 1000u);
      a.worker_requested = worker != 0u;
      assert(h2_fruit_slice_audio_pump(&a, now()) == 0);
      if (worker) delay(55u);
      assert(h2_fruit_slice_audio_close(&a) == 0);
      assert(sink.writes >= 2u);
      if (!worker)
        memcpy(expected, sink.first_pcm, sizeof(expected));
      else
        assert(memcmp(expected, sink.first_pcm, sizeof(expected)) == 0);
    }
  }
}
static void test_provider_failures(void) {
  for (unsigned failure = 0u; failure < 2u; ++failure) {
    reset_sink();
    h2_runtime_t rt = direct_runtime();
    h2_fruit_slice_audio_t a;
    assert(h2_fruit_slice_audio_init(&a, &rt) == 0);
    a.worker_requested = true;
    if (failure == 0u)
      sink.fail_create = H2_PAL_ERR_IO;
    else
      sink.fail_close = H2_PAL_ERR_IO;
    const int first = h2_fruit_slice_audio_pump(&a, now());
    assert(first == H2_PAL_OK || (failure == 0u && first == H2_PAL_ERR_IO));
    delay(40);
    assert(h2_fruit_slice_audio_pump(&a, now()) ==
           (failure == 0u ? H2_PAL_ERR_IO : H2_PAL_OK));
    const int closed = h2_fruit_slice_audio_close(&a);
    assert(closed == (failure == 0u ? H2_PAL_OK : H2_PAL_ERR_IO));
    assert(!a.worker && !a.track && live_queues == 0u);
    assert(h2_fruit_slice_audio_close(&a) == H2_PAL_OK);
    assert(sink.opens == sink.closes);
  }
}

static void test_backpressure_error_and_burst(void) {
  reset_sink();
  h2_runtime_t rt = direct_runtime();
  h2_fruit_slice_audio_t a;
  assert(h2_fruit_slice_audio_init(&a, &rt) == 0);
  a.worker_requested = true;
  sink.blocks = 3;
  sink.fail_write = 8;
  assert(h2_fruit_slice_audio_pump(&a, now()) == 0);
  for (unsigned i = 0; i < 1000; ++i)
    h2_fruit_slice_audio_emit(&a, H2_FRUIT_SLICE_SOUND_SLICE, now());
  delay(300);
  assert(h2_fruit_slice_audio_pump(&a, now()) == H2_PAL_ERR_IO);
  assert(a.worker && a.blocked_writes >= 3u);
  assert(h2_fruit_slice_audio_close(&a) == 0);
  assert(sink.opens == 1 && sink.closes == 1);
}

/* Public App test with real Runtime wrappers, quantized waits and synchronous
 * display/Pref pressure. Gameplay still receives its normal touch transitions.
 */
static unsigned frames, commits, touch_phase, display_pressure_ms;
static const unsigned event_frames[] = {5u, 35u, 40u, 45u, 65u, 85u, 90u};
static const unsigned event_buttons[] = {
    H2_FRUIT_SLICE_BUTTON_CARDS, H2_FRUIT_SLICE_BUTTON_BACK,
    H2_FRUIT_SLICE_BUTTON_TASKS, H2_FRUIT_SLICE_BUTTON_CLAIM,
    H2_FRUIT_SLICE_BUTTON_CLAIM, H2_FRUIT_SLICE_BUTTON_BACK,
    H2_FRUIT_SLICE_BUTTON_START};
static int display_open(void* u) {
  (void)u;
  return 0;
}
static int display_info(void* u, h2_display_info_t* i) {
  (void)u;
  *i = (h2_display_info_t){480, 320, H2_DISPLAY_PIXEL_RGB565};
  return 0;
}
static int draw(void* u, const h2_display_rect_t* r, const void* p,
                size_t stride, h2_display_pixel_format_t fmt) {
  (void)u;
  (void)r;
  (void)p;
  (void)stride;
  (void)fmt;
  return 0;
}
static int present(void* u) {
  (void)u;
  delay(frames == 5u || frames % 20u == 0 ? display_pressure_ms : 8u);
  return 0;
}
static const h2_pal_display_vtable_t dvt = {.open = display_open,
                                            .get_info = display_info,
                                            .draw_bitmap = draw,
                                            .present = present,
                                            .close = display_open};
static const h2_pal_display_api_t display_api = {.vtable = &dvt};
static int touch_open(void* u) {
  (void)u;
  return 0;
}
static int touch_info(void* u, h2_pal_touch_info_t* i) {
  (void)u;
  *i = (h2_pal_touch_info_t){480, 320};
  return 0;
}
static int touch_poll(void* u, h2_pal_touch_event_t* e) {
  (void)u;
  const unsigned event = touch_phase / 2u;
  if (event >= sizeof(event_frames) / sizeof(event_frames[0]) ||
      frames < event_frames[event])
    return H2_PAL_ERR_WOULD_BLOCK;
  h2_fruit_slice_game_t g;
  h2_fruit_slice_game_init(&g, 480, 320, 42, 0);
  if (event == 1u) g.phase = H2_FRUIT_SLICE_PHASE_CARDS;
  if (event >= 3u && event <= 5u) g.phase = H2_FRUIT_SLICE_PHASE_TASKS;
  h2_fruit_slice_rect_t r;
  assert(h2_fruit_slice_game_button_rect(&g, event_buttons[event], &r));
  *e = (h2_pal_touch_event_t){
      (touch_phase++ % 2u) ? H2_PAL_TOUCH_EVENT_UP : H2_PAL_TOUCH_EVENT_DOWN,
      (uint16_t)((r.x0 + r.x1) / 2), (uint16_t)((r.y0 + r.y1) / 2)};
  return 0;
}
static const h2_pal_touch_vtable_t tvt = {.open = touch_open,
                                          .get_info = touch_info,
                                          .poll_event = touch_poll,
                                          .close = touch_open};
static const h2_pal_touch_api_t touch_api = {.vtable = &tvt};
static int pref_get(h2_pal_pref_namespace_t* n, const char* k, uint32_t* v) {
  (void)n;
  (void)k;
  (void)v;
  return H2_PAL_ERR_NOT_FOUND;
}
static int pref_get_blob(h2_pal_pref_namespace_t* n, const h2_pal_mem_api_t* m,
                         const char* k, void** p, size_t* s) {
  (void)n;
  if (strcmp(k, "progress_v1") != 0) return H2_PAL_ERR_NOT_FOUND;
  const h2_fruit_slice_progress_t profile = {.fruit_credit = 100u, .owned = 1u};
  *s = H2_FRUIT_SLICE_PROGRESS_BYTES;
  *p = h2_pal_mem_alloc(m, *s);
  if (!*p) return H2_PAL_ERR_NO_MEMORY;
  h2_fruit_slice_progress_encode(&profile, *p);
  return 0;
}
static int pref_set_blob(h2_pal_pref_namespace_t* n, const char* k,
                         const void* p, size_t s) {
  (void)n;
  (void)k;
  (void)p;
  (void)s;
  return 0;
}
static int pref_commit(h2_pal_pref_namespace_t* n) {
  (void)n;
  ++commits;
  delay(39u);
  return 0;
}
static int pref_close(h2_pal_pref_namespace_t* n) {
  (void)n;
  return 0;
}
static h2_pal_pref_namespace_t ns = {.get_u32 = pref_get,
                                     .get_blob = pref_get_blob,
                                     .set_blob = pref_set_blob,
                                     .commit = pref_commit,
                                     .close = pref_close};
static int pref_open(void* u, const char* n, h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t** p) {
  (void)u;
  (void)n;
  (void)mode;
  *p = &ns;
  return 0;
}
static const h2_pal_pref_vtable_t pvt = {.open = pref_open};
static const h2_pal_pref_api_t pref_api = {.vtable = &pvt};
static bool exit_app(void* u) {
  (void)u;
  return ++frames > 120u;
}

static uint64_t run_app(bool worker, unsigned pressure) {
  reset_sink();
  frames = commits = touch_phase = 0;
  display_pressure_ms = pressure;
  h2_runtime_config_t c = {
      .board = "continuity", .chip = "host", .target = "desktop"};
#define UNSUPPORTED(name) c.name = h2_pal_unsupported_##name##_api()
  UNSUPPORTED(firmware_info);
  UNSUPPORTED(timer);
  UNSUPPORTED(fs);
  UNSUPPORTED(disk);
  UNSUPPORTED(crypto);
  UNSUPPORTED(http);
  UNSUPPORTED(net);
  UNSUPPORTED(netif);
  UNSUPPORTED(mqtt);
  UNSUPPORTED(webrtc);
  UNSUPPORTED(wifi_sta);
  UNSUPPORTED(wifi_ap);
  UNSUPPORTED(wifi_csi);
  UNSUPPORTED(wifi_settings);
  UNSUPPORTED(ble_host);
  UNSUPPORTED(modem);
  UNSUPPORTED(power);
  UNSUPPORTED(audio_decoder);
  UNSUPPORTED(periph);
  UNSUPPORTED(button);
  UNSUPPORTED(buzzer);
  UNSUPPORTED(nfc);
  UNSUPPORTED(nfc_card_emulation);
  UNSUPPORTED(imu);
  UNSUPPORTED(gpio_irq);
  UNSUPPORTED(led);
  UNSUPPORTED(pwm_switch);
  UNSUPPORTED(input);
  UNSUPPORTED(system_event);
  UNSUPPORTED(video_decoder);
#undef UNSUPPORTED
  c.switch_api = h2_pal_unsupported_switch_api();
  c.pref = &pref_api;
  c.mem = h2_desktop_platform_default_allocator();
  c.log = h2_desktop_platform_log_api();
  c.time = h2_desktop_platform_time_api();
  c.task = &task_api;
  c.queue = h2_desktop_platform_queue_api();
  c.sync = h2_desktop_platform_sync_api();
  c.display = &display_api;
  c.touch = &touch_api;
  c.audio = &audio_api;
  h2_runtime_t* rt = NULL;
  assert(h2_runtime_init(&c, &rt) == 0);
  const h2_fruit_slice_config_t cfg = {.seed = 42,
                                       .frame_period_ms = 30,
                                       .should_exit = exit_app,
                                       .prewarm_audio = true,
                                       .audio_worker = worker};
  assert(h2_fruit_slice_run(rt, &cfg) == 0);
  assert(h2_runtime_destroy(&rt) == 0);
  assert(sink.opens == sink.closes && sink.writes > 0u && commits >= 2u);
  assert(touch_phase == 2u * sizeof(event_frames) / sizeof(event_frames[0]));
  printf(
      "public-App worker=%d album_pressure=%u missing_ms=%llu refill_gap=%llu writes=%u pref_commits=%u\n",
      worker, pressure, (unsigned long long)sink.missing,
      (unsigned long long)sink.max_gap, sink.writes, commits);
  return sink.missing;
}
int main(void) {
  init_queue_api();
  test_pcm_identity();
  test_owner_lifecycle_and_latency();
  test_provider_failures();
  test_backpressure_error_and_burst();
  const unsigned pressures[] = {154u, 180u};
  for (unsigned i = 0; i < 2u; ++i) {
    const uint64_t cooperative = run_app(false, pressures[i]);
    const uint64_t independent = run_app(true, pressures[i]);
    assert(cooperative >= 200u && independent < cooperative / 4u);
  }
  puts("audio worker continuity tests passed");
}
