#include "h2_fruit_slice_audio_worker.h"

#include "h2_atomic.h"

#include <stdio.h>
#include <string.h>

#define COMMANDS 32u
#define SERVICE_MS 10u

typedef struct audio_command {
  uint64_t at_ms;
  h2_fruit_slice_sound_t sound;
  bool enable, set_mute;
  uint8_t mute;
} audio_command_t;

typedef struct audio_snapshot {
  uint32_t writes, blocked, rebases, gap_ms;
  int result;
} audio_snapshot_t;

struct h2_fruit_slice_audio_worker {
  h2_fruit_slice_audio_t mixer; /* Only the worker accesses this until join. */
  h2_pal_queue_t* commands;
  h2_pal_queue_t* snapshots;
  h2_pal_task_t* task;
  audio_snapshot_t seen; /* Only the main thread accesses these fields. */
  bool enabled, stop_requested, close_confirmed;
  /* A single short-lived Pref task prevents storage I/O from starving PCM.
   * Arguments are immutable until join; done publishes its result. */
  h2_pal_task_t* pref_task;
  h2_atomic_bool_t pref_done;
  uint8_t saving_mute;
  int pref_result;
  uint64_t pref_after_ms, drain_after_ms;
  bool pref_dirty, pref_completed, closing;
};

static audio_snapshot_t snapshot(const h2_fruit_slice_audio_t* audio,
                                 int result) {
  return (audio_snapshot_t){.writes = audio->writes,
                            .blocked = audio->blocked_writes,
                            .rebases = audio->rebases,
                            .gap_ms = audio->max_pump_gap_ms,
                            .result = result};
}

static void apply_snapshot(h2_fruit_slice_audio_t* audio,
                           const audio_snapshot_t* current) {
  struct h2_fruit_slice_audio_worker* w = audio->worker;
  audio->writes += current->writes - w->seen.writes;
  audio->blocked_writes += current->blocked - w->seen.blocked;
  audio->rebases += current->rebases - w->seen.rebases;
  if (current->gap_ms > audio->max_pump_gap_ms)
    audio->max_pump_gap_ms = current->gap_ms;
  w->seen = *current;
}

static void report_control(h2_fruit_slice_audio_t* audio, const char* what,
                           int rc) {
  char message[96];
  (void)snprintf(message, sizeof(message), "H2_FRUIT_SLICE_AUDIO_%s rc=%d",
                 what, rc);
  (void)h2_pal_log_write(audio->runtime->log, H2_PAL_LOG_WARN, "fruit-slice",
                         message);
}

static void save_preferences(void* user) {
  struct h2_fruit_slice_audio_worker* w = user;
  w->pref_result =
      h2_fruit_slice_audio_save_mute(w->mixer.runtime, w->saving_mute);
  h2_atomic_bool_store(&w->pref_done, true, H2_ATOMIC_RELEASE);
}

static void service_preferences(struct h2_fruit_slice_audio_worker* w,
                                uint64_t now, bool finishing) {
  h2_runtime_t* rt = w->mixer.runtime;
  if (w->pref_task != NULL) {
    if (!finishing && !h2_atomic_bool_load(&w->pref_done, H2_ATOMIC_ACQUIRE))
      return;
    const int joined = h2_pal_task_join(rt->task, w->pref_task);
    if (joined != H2_PAL_OK) {
      report_control(&w->mixer, "PREF_JOIN_RETRY", joined);
      return;
    }
    w->pref_task = NULL;
    w->pref_completed = w->saving_mute == w->mixer.mute;
    if (w->pref_result != H2_PAL_OK)
      report_control(&w->mixer, "PREF_SAVE", w->pref_result);
    if ((w->pref_result == H2_PAL_OK ||
         w->pref_result == H2_PAL_ERR_UNSUPPORTED) &&
        w->saving_mute == w->mixer.mute)
      w->pref_dirty = false;
    w->pref_after_ms = now + 500u;
  }
  if (!w->pref_dirty || (!finishing && now < w->pref_after_ms) ||
      (finishing && w->pref_completed))
    return;
  w->saving_mute = w->mixer.mute;
  h2_atomic_bool_store(&w->pref_done, false, H2_ATOMIC_RELEASE);
  const h2_pal_task_options_t options = {.name = "fruit-slice/audio-pref",
                                         .min_stack_size = 8192u};
  const int rc =
      h2_pal_task_start(rt->task, &options, save_preferences, w, &w->pref_task);
  if (rc != H2_PAL_OK) {
    report_control(&w->mixer, "PREF_START_RETRY", rc);
    w->pref_after_ms = now + 500u;
    if (finishing) w->pref_completed = true;
  }
}

static void worker_run(void* user) {
  struct h2_fruit_slice_audio_worker* w = user;
  h2_fruit_slice_audio_t* audio = &w->mixer;
  const h2_pal_queue_api_t* queue = audio->runtime->queue;
  bool enabled = false;
  int result = H2_PAL_OK;
  for (;;) {
    audio_command_t command;
    const bool service =
        enabled && (audio->mute != 3u || audio->track != NULL ||
                    audio->speaker_started || w->pref_dirty || w->pref_task);
    int received = h2_pal_queue_recv(queue, w->commands, &command,
                                     service ? SERVICE_MS : UINT32_MAX);
    bool changed_mute = false;
    for (unsigned i = 0u; received == H2_PAL_OK; ++i) {
      if (command.set_mute) {
        h2_fruit_slice_audio_apply_mute(audio, command.mute);
        w->pref_dirty = true;
        w->pref_completed = false;
        changed_mute = true;
        w->drain_after_ms = 0u;
      } else if (result == H2_PAL_OK) {
        if (command.enable)
          enabled = true;
        else
          h2_fruit_slice_audio_emit(audio, command.sound, command.at_ms);
      }
      if (i + 1u == COMMANDS) break;
      received = h2_pal_queue_recv(queue, w->commands, &command, 0u);
    }
    uint64_t now = 0u;
    const int timed = h2_pal_time_get_monotonic_ms(audio->runtime->time, &now);
    if (changed_mute) w->pref_after_ms = now + 100u; /* Coalesce rapid taps. */
    if (received == H2_PAL_ERR_CLOSED) {
      /* Flush the latest accepted setting once, and retain its context until
       * any started Pref task has really joined. Persistence errors are logged
       * without trapping App exit in an endless save retry. */
      w->pref_completed = false;
      do {
        service_preferences(w, now, true);
        if (w->pref_task != NULL)
          (void)h2_pal_time_sleep_ms(audio->runtime->time, SERVICE_MS);
      } while (w->pref_task != NULL);
      break;
    }
    const bool receive_failed = received != H2_PAL_OK &&
                                received != H2_PAL_ERR_WOULD_BLOCK &&
                                received != H2_PAL_ERR_TIMEOUT;
    if (receive_failed) result = received;
    if (timed != H2_PAL_OK) result = timed;
    /* Full mute keeps only the existing bounded controller task idle. Its
     * track/speaker are released after a zero-ending fade, including retries.
     * A quick unmute can reverse the fade without a DAC close/open cycle. */
    if (w->closing) {
      const int closed = h2_fruit_slice_audio_suspend(audio);
      if (closed != H2_PAL_OK)
        report_control(audio, "MUTE_CLOSE_RETRY", closed);
      w->closing = audio->track != NULL || audio->speaker_started;
    }
    if (!w->closing && audio->mute == 3u &&
        (!enabled || audio->ramp_left == 0u)) {
      if (audio->track != NULL && enabled && w->drain_after_ms == 0u)
        w->drain_after_ms = now + 100u;
      int drained = audio->track != NULL && enabled
                        ? h2_pal_audio_track_drain(audio->track, 0u)
                        : H2_PAL_OK;
      const bool waiting = (drained == H2_PAL_ERR_WOULD_BLOCK ||
                            drained == H2_PAL_ERR_TIMEOUT) &&
                           now < w->drain_after_ms;
      if (!waiting && (audio->track || audio->speaker_started)) {
        const int closed = h2_fruit_slice_audio_suspend(audio);
        if (closed != H2_PAL_OK)
          report_control(audio, "MUTE_CLOSE_RETRY", closed);
        w->closing = audio->track != NULL || audio->speaker_started;
      }
    } else if (!w->closing && enabled && result == H2_PAL_OK) {
      result = h2_fruit_slice_audio_pump(audio, now);
    }
    service_preferences(w, now, false);
    const audio_snapshot_t current = snapshot(audio, result);
    (void)h2_pal_queue_send_latest(queue, w->snapshots, &current);
    if (receive_failed) {
      while (w->pref_task != NULL) {
        service_preferences(w, now, true);
        if (w->pref_task)
          (void)h2_pal_time_sleep_ms(audio->runtime->time, SERVICE_MS);
      }
      return;
    }
    if (result != H2_PAL_OK) enabled = false;
  }
}

int h2_fruit_slice_audio_worker_start(h2_fruit_slice_audio_t* audio) {
  if (audio->worker != NULL) return H2_PAL_OK;
  const h2_pal_queue_api_t* queue = audio->runtime->queue;
  const h2_pal_task_api_t* tasks = audio->runtime->task;
  /* Do not create a producer that this Runtime cannot stop and join. */
  if (queue == NULL || queue->vtable == NULL || queue->vtable->create == NULL ||
      queue->vtable->destroy == NULL || queue->vtable->send == NULL ||
      queue->vtable->recv == NULL || queue->vtable->send_latest == NULL ||
      queue->vtable->close == NULL || tasks == NULL || tasks->vtable == NULL ||
      tasks->vtable->start == NULL || tasks->vtable->join == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  struct h2_fruit_slice_audio_worker* w =
      h2_pal_mem_alloc(audio->runtime->mem, sizeof(*w));
  if (w == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(w, 0, sizeof(*w));
  w->mixer = *audio;
  w->mixer.worker_requested = false;
  w->seen = snapshot(audio, H2_PAL_OK);
  h2_pal_queue_config_t config = {.name = "fruit-audio-commands",
                                  .item_size = sizeof(audio_command_t),
                                  .item_count = COMMANDS,
                                  .allocator = audio->runtime->mem};
  int rc = h2_atomic_bool_init(&w->pref_done, false);
  if (rc != H2_ATOMIC_OK) {
    h2_pal_mem_free(audio->runtime->mem, w);
    return H2_PAL_ERR_NO_MEMORY;
  }
  rc = h2_pal_queue_create(queue, &config, &w->commands);
  if (rc == H2_PAL_OK) {
    config.name = "fruit-audio-stats";
    config.item_size = sizeof(audio_snapshot_t);
    config.item_count = 1u;
    rc = h2_pal_queue_create(queue, &config, &w->snapshots);
  }
  if (rc == H2_PAL_OK) {
    const h2_pal_task_options_t task = {.name = "fruit-slice/audio",
                                        .min_stack_size = 8192u};
    rc =
        h2_pal_task_start(audio->runtime->task, &task, worker_run, w, &w->task);
  }
  if (rc != H2_PAL_OK) {
    h2_pal_queue_destroy(queue, w->snapshots);
    h2_pal_queue_destroy(queue, w->commands);
    h2_atomic_bool_destroy(&w->pref_done);
    h2_pal_mem_free(audio->runtime->mem, w);
    return rc;
  }
  /* The task starts idle. Ownership transfers only after successful start;
   * every later provider call, including close, waits for ownership return. */
  audio->worker = w;
  audio->pcm = NULL;
  audio->track = NULL;
  audio->speaker_started = false;
  return H2_PAL_OK;
}

int h2_fruit_slice_audio_worker_mute(h2_fruit_slice_audio_t* audio,
                                     uint8_t mute) {
  struct h2_fruit_slice_audio_worker* w = audio->worker;
  if (w->stop_requested) return H2_PAL_ERR_BUSY;
  const audio_command_t command = {.set_mute = true, .mute = mute};
  const int rc =
      h2_pal_queue_send(audio->runtime->queue, w->commands, &command, 0u);
  if (rc == H2_PAL_OK) audio->mute = mute;
  return rc;
}

void h2_fruit_slice_audio_worker_emit(h2_fruit_slice_audio_t* audio,
                                      h2_fruit_slice_sound_t sound,
                                      uint64_t now_ms) {
  struct h2_fruit_slice_audio_worker* w = audio->worker;
  if (w->stop_requested) {
    ++audio->dropped_commands;
    return;
  }
  const audio_command_t command = {.at_ms = now_ms, .sound = sound};
  if (h2_pal_queue_send(audio->runtime->queue, w->commands, &command, 0u) !=
      H2_PAL_OK)
    ++audio->dropped_commands;
}

int h2_fruit_slice_audio_worker_poll(h2_fruit_slice_audio_t* audio) {
  struct h2_fruit_slice_audio_worker* w = audio->worker;
  /* Close can fail after changing the queue predicate but before waking its
   * receiver. Suppress activation and polling from the first stop request;
   * the caller must retry close/join without permanently disabling audio. */
  if (w->stop_requested) return H2_PAL_OK;
  if (!w->enabled) {
    const audio_command_t command = {.enable = true};
    int rc =
        h2_pal_queue_send(audio->runtime->queue, w->commands, &command, 0u);
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT ||
        rc == H2_PAL_ERR_FULL)
      return H2_PAL_OK; /* Retry activation on the next main-loop service. */
    if (rc != H2_PAL_OK) return rc;
    w->enabled = true;
  }
  audio_snapshot_t current;
  const int rc =
      h2_pal_queue_recv(audio->runtime->queue, w->snapshots, &current, 0u);
  if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT)
    return w->seen.result;
  if (rc != H2_PAL_OK) return rc;
  apply_snapshot(audio, &current);
  return current.result;
}

int h2_fruit_slice_audio_worker_stop(h2_fruit_slice_audio_t* audio) {
  struct h2_fruit_slice_audio_worker* w = audio->worker;
  if (w == NULL) return H2_PAL_OK;
  w->stop_requested = true;
  if (!w->close_confirmed) {
    int rc = h2_pal_queue_close(audio->runtime->queue, w->commands);
    if (rc != H2_PAL_OK) return rc;
    /* Only successful close confirms all blocked queue users were notified.
     * Joining earlier could deadlock an idle producer after a failed wake. */
    w->close_confirmed = true;
  }
  int rc = h2_pal_task_join(audio->runtime->task, w->task);
  if (rc != H2_PAL_OK) return rc;
  const audio_snapshot_t final = snapshot(&w->mixer, w->seen.result);
  apply_snapshot(audio, &final);
  const uint32_t writes = audio->writes, blocked = audio->blocked_writes;
  const uint32_t rebases = audio->rebases, gap = audio->max_pump_gap_ms;
  const uint32_t dropped = audio->dropped_commands;
  const bool requested = audio->worker_requested;
  *audio = w->mixer;
  audio->writes = writes;
  audio->blocked_writes = blocked;
  audio->rebases = rebases;
  audio->max_pump_gap_ms = gap;
  audio->dropped_commands = dropped;
  audio->worker_requested = requested;
  h2_pal_queue_destroy(audio->runtime->queue, w->snapshots);
  h2_pal_queue_destroy(audio->runtime->queue, w->commands);
  h2_atomic_bool_destroy(&w->pref_done);
  h2_pal_mem_free(audio->runtime->mem, w);
  return H2_PAL_OK;
}
