#include "h2_audio_stop_restart_config.h"
#include "h2_jieli_ac791n_devkit.h"
#include "h2_runtime.h"
#include "h2_smoke_audio_system.h"

#include "h2_atomic.h"
#include <stdio.h>

#define H2_AUDIO_STOP_RESTART_STREAM_MS 3000u
#define H2_AUDIO_STOP_RESTART_SETTLE_MS 2000u

_Static_assert(H2_AUDIO_STOP_RESTART_CYCLES > 0u, "at least one audio cycle is required");

extern uint32_t get_malloc_remain_heap_size(void);
extern int os_tasks_num_query(void);

typedef struct audio_cycle_state {
  h2_runtime_t *runtime;
  h2_pal_task_t *task;
  h2_atomic_bool_t started;
  h2_atomic_bool_t entry_released;
  h2_atomic_bool_t stop_requested;
  h2_atomic_int_t startup_result;
  uint32_t heap_baseline;
  int tasks_baseline;
} audio_cycle_state_t;

static audio_cycle_state_t audio_cycle;

static void audio_cycle_atomic_destroy(void) {
  h2_atomic_destroy(&audio_cycle.started);
  h2_atomic_destroy(&audio_cycle.entry_released);
  h2_atomic_destroy(&audio_cycle.stop_requested);
  h2_atomic_destroy(&audio_cycle.startup_result);
}

static bool audio_cycle_atomic_init(void) {
  if (h2_atomic_init(&audio_cycle.started, false) != H2_ATOMIC_OK ||
      h2_atomic_init(&audio_cycle.entry_released, false) != H2_ATOMIC_OK ||
      h2_atomic_init(&audio_cycle.stop_requested, false) != H2_ATOMIC_OK ||
      h2_atomic_init(&audio_cycle.startup_result, H2_PAL_ERR_INVALID_STATE) != H2_ATOMIC_OK) {
    audio_cycle_atomic_destroy();
    return false;
  }
  return true;
}


static int provider_idle(const h2_jieli_ac791n_devkit_audio_idle_t *probe) {
  return probe->open_tracks == 0u && probe->retained_operations == 0u &&
         probe->ring_bytes == 0u && probe->sdk_servers == 0u &&
         probe->mic_open == 0u && probe->speaker_started == 0u;
}

static void audio_cycle_task(void *user) {
  audio_cycle_state_t *state = user;
  h2_runtime_t *runtime = state->runtime;
  const h2_smoke_audio_system_config_t config = {
      .music_path = NULL, .speaker_volume_percent = 80u};
  uint32_t heap_ref = state->heap_baseline, heap_last = heap_ref, heap_min = UINT32_MAX;
  int tasks_ref = state->tasks_baseline, tasks_max = 0;
  unsigned ok = 0u, failed = 0u;
  int stop_result = H2_AUDIO_OK;
  for (unsigned cycle = 1u; cycle <= H2_AUDIO_STOP_RESTART_CYCLES; ++cycle) {
    int run_result = h2_smoke_audio_system_run(runtime, &config);
    if (cycle == 1u) {
      h2_atomic_store_explicit(&state->startup_result, run_result, H2_ATOMIC_RELEASE);
      h2_atomic_store_explicit(&state->started, true, H2_ATOMIC_RELEASE);
    }
    h2_smoke_audio_system_stats_t stats = {0};
    h2_jieli_ac791n_devkit_audio_idle_t streaming = {0}, stopped = {0};
    int stream_probe = H2_PAL_ERR_INVALID_STATE;
    if (run_result == H2_AUDIO_OK &&
        !h2_atomic_load_explicit(&state->stop_requested, H2_ATOMIC_ACQUIRE)) {
      (void)h2_pal_time_sleep_ms(runtime->time, H2_AUDIO_STOP_RESTART_STREAM_MS);
      h2_smoke_audio_system_get_stats(&stats);
      stream_probe = h2_jieli_ac791n_devkit_audio_idle_probe(&streaming);
    }
    uint64_t stop_start = 0u, stop_end = 0u;
    (void)h2_pal_time_get_monotonic_ms(runtime->time, &stop_start);
    for (unsigned attempt = 0u; attempt < 100u; ++attempt) {
      stop_result = h2_smoke_audio_system_stop();
      if (stop_result == H2_AUDIO_OK) break;
      if (attempt + 1u < 100u) (void)h2_pal_time_sleep_ms(runtime->time, 10u);
    }
    (void)h2_pal_time_get_monotonic_ms(runtime->time, &stop_end);
    int idle = h2_jieli_ac791n_devkit_audio_idle_probe(&stopped) == H2_AUDIO_OK &&
               provider_idle(&stopped);
    int tasks = os_tasks_num_query();
    for (unsigned elapsed = 0u; elapsed < H2_AUDIO_STOP_RESTART_SETTLE_MS; elapsed += 10u) {
      /* Cycle 1 establishes the post-SDK reference after a full settle window. */
      if (cycle != 1u && tasks == tasks_ref) break;
      (void)h2_pal_time_sleep_ms(runtime->time, 10u);
      tasks = os_tasks_num_query();
    }
    heap_last = get_malloc_remain_heap_size();
    if (cycle == 1u) { heap_ref = heap_last; tasks_ref = tasks; }
    if (heap_last < heap_min) heap_min = heap_last;
    if (tasks > tasks_max) tasks_max = tasks;
    int passed = run_result == H2_AUDIO_OK && stats.mic_frames > 0u &&
                 stats.music_frames > 0u && stream_probe == H2_AUDIO_OK &&
                 streaming.consumed_bytes > 0u && stop_result == H2_AUDIO_OK &&
                 idle && tasks == tasks_ref;
    if (passed) ++ok; else ++failed;
    printf("H2_JIELI_AUDIO_CYCLE cycle=%u/%u run=%d mic_frames=%lu music_frames=%lu consumed=%llu stop=%d stop_ms=%llu idle=%d heap_free=%lu tasks=%d result=%s\n",
           cycle, H2_AUDIO_STOP_RESTART_CYCLES, run_result,
           (unsigned long)stats.mic_frames, (unsigned long)stats.music_frames,
           (unsigned long long)streaming.consumed_bytes, stop_result,
           (unsigned long long)(stop_end - stop_start), idle,
           (unsigned long)heap_last, tasks, passed ? "ok" : "fail");
    if (run_result != H2_AUDIO_OK || stop_result != H2_AUDIO_OK ||
        h2_atomic_load_explicit(&state->stop_requested, H2_ATOMIC_ACQUIRE)) break;
  }
  if (heap_min == UINT32_MAX) heap_min = heap_last;
  printf("H2_JIELI_AUDIO_CYCLE_SUMMARY cycles=%u ok=%u failed=%u heap_baseline=%lu heap_ref=%lu heap_last=%lu heap_min=%lu tasks_baseline=%d tasks_ref=%d tasks_max=%d result=%s\n",
         H2_AUDIO_STOP_RESTART_CYCLES, ok, failed,
         (unsigned long)state->heap_baseline, (unsigned long)heap_ref,
         (unsigned long)heap_last, (unsigned long)heap_min,
         state->tasks_baseline, tasks_ref, tasks_max,
         ok == H2_AUDIO_STOP_RESTART_CYCLES && failed == 0u && heap_last >= heap_ref ? "ok" : "fail");
  /* Entry borrows Runtime until it publishes the startup result to the launcher. */
  while (!h2_atomic_load_explicit(&state->entry_released, H2_ATOMIC_ACQUIRE)) {
    (void)h2_pal_time_sleep_ms(runtime->time, 10u);
  }
  if (stop_result == H2_AUDIO_OK) {
    h2_runtime_deinit(runtime);
    state->runtime = NULL;
  } else {
    printf("H2_JIELI_AUDIO_SYSTEM cleanup did not complete; Runtime intentionally retained result=%d attempts=100\n",
           stop_result);
  }
  audio_cycle_atomic_destroy();
}

int h2_jieli_target_application_run(void) {
  h2_runtime_config_t config;
  h2_runtime_t *runtime = NULL;
  if (!audio_cycle_atomic_init()) return H2_PAL_ERR_NO_MEMORY;
  int result = h2_jieli_ac791n_devkit_runtime_config(&config);
  if (result == H2_PAL_OK) result = h2_runtime_init(&config, &runtime);
  if (result != H2_PAL_OK) {
    printf("H2_JIELI_AUDIO_CYCLE stage=runtime-init result=%d\n", result);
    if (runtime != NULL) h2_runtime_deinit(runtime);
    audio_cycle_atomic_destroy();
    return result;
  }
  audio_cycle.runtime = runtime;
  audio_cycle.heap_baseline = get_malloc_remain_heap_size();
  audio_cycle.tasks_baseline = os_tasks_num_query();
  printf("H2_JIELI_AUDIO_CYCLE baseline heap_free=%lu tasks=%d cycles=%u\n",
         (unsigned long)audio_cycle.heap_baseline, audio_cycle.tasks_baseline,
         H2_AUDIO_STOP_RESTART_CYCLES);
  const h2_pal_task_options_t options = {
      .name = "audio-cycle", .min_stack_size = 32768u};
  result = h2_pal_task_start(runtime->task, &options, audio_cycle_task,
                           &audio_cycle, &audio_cycle.task);
  if (result != H2_PAL_OK) {
    h2_runtime_deinit(runtime);
    audio_cycle.runtime = NULL;
    audio_cycle_atomic_destroy();
    return result;
  }
  result = H2_PAL_ERR_TIMEOUT;
  for (unsigned attempt = 0u; attempt < 1000u; ++attempt) {
    if (h2_atomic_load_explicit(&audio_cycle.started, H2_ATOMIC_ACQUIRE)) {
      result = h2_atomic_load_explicit(&audio_cycle.startup_result, H2_ATOMIC_ACQUIRE);
      break;
    }
    (void)h2_pal_time_sleep_ms(runtime->time, 10u);
  }
  if (result == H2_PAL_ERR_TIMEOUT) {
    h2_atomic_store_explicit(&audio_cycle.stop_requested, true, H2_ATOMIC_RELEASE);
  }
  printf("H2_JIELI_AUDIO_CYCLE_READY cycles=%u result=%d\n", H2_AUDIO_STOP_RESTART_CYCLES, result);
  h2_atomic_store_explicit(&audio_cycle.entry_released, true, H2_ATOMIC_RELEASE);
  return result;
}
