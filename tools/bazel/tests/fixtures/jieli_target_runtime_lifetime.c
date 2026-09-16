#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define H2_AUDIO_STOP_RESTART_CYCLES 3u
#define H2_PAL_OK 0
#define H2_AUDIO_OK 0
#define H2_PAL_ERR_INVALID_STATE -7
#define H2_PAL_ERR_TIMEOUT -8
#define H2_PAL_ERR_NOT_FOUND -9
#define H2_SMOKE_MP4_PLAYER_DISPLAY_CENTER 0
typedef int h2_pal_result_t;
typedef struct { int unused; } h2_pal_task_t;
typedef struct { const void *task, *time, *display, *fs; } h2_runtime_t;
typedef struct { const void *periph, *component_mapper; } h2_runtime_config_t;
typedef struct { const char *name; unsigned min_stack_size; } h2_pal_task_options_t;
typedef struct { int unused; } h2_pal_fs_stat_t;
typedef struct { const char *music_path; unsigned speaker_volume_percent; } h2_smoke_audio_system_config_t;
typedef struct { void (*crash)(void *); } h2_crash_before_confirm_config_t;
typedef struct {
 unsigned width, height; const void *buttons; size_t button_count;
 int (*should_stop)(void *); void *stop_user;
 void (*on_started)(void *, int); void *started_user;
} smoke_config_t;
typedef smoke_config_t h2_button_smoke_config_t;
typedef smoke_config_t h2_touch_smoke_config_t;
typedef struct {
 const char *media_path; unsigned acquire_timeout_ms; int looping, display_mode, require_audio;
 int (*should_stop)(void *); int (*on_ready)(void *); void *ready_user;
} h2_smoke_mp4_player_config_t;
typedef struct { uint32_t suite_mask; } h2_pal_e2e_config_t;
typedef struct { size_t case_count, passed, failed; struct { unsigned case_id; int result; } cases[1]; void *retained_cleanup; } h2_pal_e2e_result_t;
static h2_runtime_t instance;
static atomic_int live, finishes;
static atomic_int sleep_calls;
static int fault, inits, deinits, retained, audio_active, audio_stop_calls, stop_attempts, audio_runs;
typedef struct { uint32_t mic_frames, music_frames; } h2_smoke_audio_system_stats_t;
typedef struct {
 uint32_t open_tracks, retained_operations, ring_bytes, sdk_servers, mic_open, speaker_started;
 uint64_t consumed_bytes;
} h2_jieli_ac791n_devkit_audio_idle_t;
int os_tasks_num_query(void) { assert(live); return 12; }
int h2_jieli_ac791n_devkit_audio_idle_probe(h2_jieli_ac791n_devkit_audio_idle_t *out) {
 assert(live); memset(out, 0, sizeof(*out));
 if (audio_active) { out->open_tracks = 2u; out->consumed_bytes = 640u; }
 return 0;
}
void h2_smoke_audio_system_get_stats(h2_smoke_audio_system_stats_t *out) {
 assert(live && audio_active); *out = (h2_smoke_audio_system_stats_t){1u, 1u};
}
int h2_pal_time_get_monotonic_ms(const void *time, uint64_t *out) {
 (void)time; assert(live); *out = (uint64_t)atomic_load(&sleep_calls) * 10u; return 0;
}
#if WORKER_TARGET
#if !CYCLE_TARGET
static const int periph_api = 0, component_mapper = 0;
const int app_buttons[1] = {0};
#endif
static pthread_t worker;
static atomic_int allow_finish;
static void (*worker_fn)(void *);
static void *worker_arg;
static int worker_started;
#endif
void emit(const char *format, ...) { (void)format; }
void trace(const char *format, ...) { (void)format; }
void boot_marker(unsigned stage, int result) { (void)stage; (void)result; }
unsigned get_malloc_remain_heap_size(void) { return 123456u; }
void os_time_dly(unsigned ticks) { (void)ticks; struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
int h2_jieli_ac791n_devkit_runtime_config(h2_runtime_config_t *c) { memset(c, 0, sizeof(*c)); return fault == 1 ? -11 : 0; }
int mp4_runtime_config(h2_runtime_config_t *c) { return h2_jieli_ac791n_devkit_runtime_config(c); }
int h2_runtime_init(const h2_runtime_config_t *c, h2_runtime_t **out) {
 (void)c; if (fault == 2) return -12;
 assert(!atomic_exchange(&live, 1)); ++inits; *out = &instance; return 0;
}
void h2_runtime_deinit(h2_runtime_t *r) {
 assert(r == &instance && !retained && !audio_active); assert(atomic_exchange(&live, 0)); ++deinits;
}
int h2_runtime_input_start(h2_runtime_t *r, const void *c) { (void)c; assert(r == &instance && live); return fault == 3 ? -13 : 0; }
int h2_pal_time_sleep_ms(const void *time, unsigned ms) { (void)time; assert(ms == 10u || (CYCLE_TARGET && (ms == 3000u || ms == 2000u))); ++sleep_calls; assert(live); os_time_dly(1); return 0; }
int h2_smoke_audio_system_run(h2_runtime_t *r, const h2_smoke_audio_system_config_t *c) { (void)c; assert(r == &instance && live); assert(!audio_active); audio_active = 1; stop_attempts = 0; ++audio_runs; return fault == 5 || (!CYCLE_TARGET && (fault == 4 || fault == 7)) ? -15 : 0; }
int h2_smoke_audio_system_stop(void) {
 assert(live && audio_active);
 ++audio_stop_calls;
 assert(++stop_attempts <= 100); /* Fail fast on the old unbounded loop. */
 if (fault == 7 || (audio_stop_calls == 1 && fault == 4)) return -16;
 audio_active = 0; return 0;
}
void crash_now(void *user) { (void)user; }
void cpu_assert_debug(void) { assert(live); }
int h2_crash_before_confirm_run(h2_runtime_t *r, const h2_crash_before_confirm_config_t *c) { (void)c; assert(r == &instance && live); return -15; }
int h2_pal_display_open(const void *d) { (void)d; assert(live); return fault == 3 ? -13 : 0; }
int h2_pal_fs_stat(const void *fs, const char *path, h2_pal_fs_stat_t *st) { (void)fs; (void)path; (void)st; assert(live); return H2_PAL_ERR_NOT_FOUND; }
int mp4_watchdog_poll(void *u) { (void)u; return 0; }
int confirm_ready(void *u) { (void)u; return 0; }
int h2_smoke_mp4_player_run(h2_runtime_t *r, const h2_smoke_mp4_player_config_t *c) { (void)c; assert(r == &instance && live); return fault == 5 ? -15 : 0; }
int h2_pal_e2e_run(h2_runtime_t *r, const h2_pal_e2e_config_t *c, h2_pal_e2e_result_t *report) {
 (void)c; assert(r == &instance && live);
 if (fault == 5) { report->retained_cleanup = report; retained = 2; return -15; }
 return 0;
}
int h2_pal_e2e_cleanup(h2_runtime_t *r, h2_pal_e2e_result_t *report) {
 assert(r == &instance && live && retained); if (--retained == 0) report->retained_cleanup = NULL; return retained ? -6 : 0;
}
#if WORKER_TARGET
static void *run_worker(void *arg) { (void)arg; worker_fn(worker_arg); atomic_store(&finishes, 1); return NULL; }
int h2_pal_task_start(const void *api, const h2_pal_task_options_t *options, void (*fn)(void *), void *arg, h2_pal_task_t **out) {
 (void)api; (void)options; (void)out; if (fault == 4 && !CYCLE_TARGET) return -14;
 if (CYCLE_TARGET) { assert(strcmp(options->name, "audio-cycle") == 0); assert(options->min_stack_size == 32768u); }
 worker_fn = fn; worker_arg = arg; worker_started = 1; assert(pthread_create(&worker, NULL, run_worker, NULL) == 0); return 0;
}
static int smoke_run(h2_runtime_t *r, const smoke_config_t *c) {
 assert(r == &instance && live);
 if (fault == 6) { while (!c->should_stop(c->stop_user)) os_time_dly(1); return -8; }
 c->on_started(c->started_user, fault == 5 ? -15 : 0);
 if (fault != 5) while (!atomic_load(&allow_finish)) { assert(live); os_time_dly(1); }
 return fault == 5 ? -15 : 0;
}
int h2_button_smoke_run(h2_runtime_t *r, const smoke_config_t *c) { return smoke_run(r, c); }
int h2_touch_smoke_run(h2_runtime_t *r, const smoke_config_t *c) { return smoke_run(r, c); }
#endif
/* TARGET */
int main(void) {
 const int faults[] = {7, 5, 1, 2, 3, 4, 6, 0};
 for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
  fault = faults[i];
  atomic_store(&sleep_calls, 0);
  inits = deinits = retained = audio_active = audio_stop_calls = stop_attempts = audio_runs = 0; atomic_store(&live, 0); atomic_store(&finishes, 0);
#if WORKER_TARGET
  worker_started = 0; atomic_store(&allow_finish, 0);
#endif
  int result = h2_jieli_target_application_run();
  if (fault == 1) assert(result == -11);
  else if (fault == 2) assert(result == -12);
#if CYCLE_TARGET
  else if (fault == 5) assert(result == -15);
  else assert(result == 0);
  if (worker_started) assert(pthread_join(worker, NULL) == 0);
#elif WORKER_TARGET
  else if (fault == 3) assert(result == -13);
  else if (fault == 4) assert(result == -14);
  else if (fault == 5) assert(result == -15);
  else if (fault == 6) assert(result == H2_PAL_ERR_TIMEOUT);
  else { assert(result == 0 && live); }
  atomic_store(&allow_finish, 1);
  if (worker_started) assert(pthread_join(worker, NULL) == 0);
#else
  else if (CRASH_TARGET) assert(result == H2_PAL_ERR_INVALID_STATE);
  else if (fault == 5 || (AUDIO_TARGET && (fault == 4 || fault == 7))) assert(result == -15);
  else assert(result == 0 || (fault == 3 && result == -13));
#endif
#if CYCLE_TARGET
  if (fault == 7) {
   assert(inits == 1 && deinits == 0 && live && audio_active);
   assert(audio_runs == 1 && audio_stop_calls == 100);
  } else {
   assert(inits == deinits && !live && !audio_active);
   if (fault == 5) assert(audio_runs == 1 && audio_stop_calls == 1);
   else if (fault != 1 && fault != 2) {
    assert(inits == 1 && audio_runs == H2_AUDIO_STOP_RESTART_CYCLES);
    assert(audio_stop_calls == (int)H2_AUDIO_STOP_RESTART_CYCLES + (fault == 4));
   }
  }
#elif AUDIO_TARGET
  if (fault == 1 || fault == 2) {
   assert(!audio_stop_calls && !sleep_calls && !deinits && !inits);
  }
  if (result == H2_AUDIO_OK) {
   /* Successful entry hands the still-running scene to the boot lifetime. */
   assert(inits == 1 && deinits == 0 && live && audio_active && !audio_stop_calls);
  } else if (fault == 7) {
   assert(result == -15 && audio_stop_calls == 100 && sleep_calls == 99);
   assert(inits == 1 && deinits == 0 && live && audio_active);
  } else {
   assert(inits == deinits && !live && !audio_active);
   if (fault == 4) assert(audio_stop_calls == 2);
  }
#else
  assert(inits == deinits && !live && !retained);
#endif
 }
 return 0;
}
