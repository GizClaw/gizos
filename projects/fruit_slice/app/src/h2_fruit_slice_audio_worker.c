#include "h2_fruit_slice_audio_worker.h"

#include <string.h>

#define COMMANDS 32u
#define SERVICE_MS 10u

typedef struct audio_command {
  uint64_t at_ms;
  h2_fruit_slice_sound_t sound;
  bool enable;
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
  bool enabled, closing;
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

static void worker_run(void* user) {
  struct h2_fruit_slice_audio_worker* w = user;
  h2_fruit_slice_audio_t* audio = &w->mixer;
  const h2_pal_queue_api_t* queue = audio->runtime->queue;
  bool enabled = false;
  int result = H2_PAL_OK;
  for (;;) {
    audio_command_t command;
    int received = h2_pal_queue_recv(queue, w->commands, &command,
                                     enabled ? SERVICE_MS : UINT32_MAX);
    /* Drain the bounded queue before mixing; close drains accepted commands
     * then exits without replaying a last block during pause/mute/teardown. */
    for (unsigned i = 0u; received == H2_PAL_OK; ++i) {
      if (result == H2_PAL_OK) {
        if (command.enable)
          enabled = true;
        else
          h2_fruit_slice_audio_emit(audio, command.sound, command.at_ms);
      }
      if (i + 1u == COMMANDS) break;
      received = h2_pal_queue_recv(queue, w->commands, &command, 0u);
    }
    if (received == H2_PAL_ERR_CLOSED) break;
    const bool receive_failed = received != H2_PAL_OK &&
                                received != H2_PAL_ERR_WOULD_BLOCK &&
                                received != H2_PAL_ERR_TIMEOUT;
    if (receive_failed) result = received;
    if (enabled && result == H2_PAL_OK) {
      uint64_t now = 0u;
      result = h2_pal_time_get_monotonic_ms(audio->runtime->time, &now);
      if (result == H2_PAL_OK) result = h2_fruit_slice_audio_pump(audio, now);
    }
    const audio_snapshot_t current = snapshot(audio, result);
    (void)h2_pal_queue_send_latest(queue, w->snapshots, &current);
    if (receive_failed) return; /* Do not spin on a broken queue. */
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
  int rc = h2_pal_queue_create(queue, &config, &w->commands);
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

void h2_fruit_slice_audio_worker_emit(h2_fruit_slice_audio_t* audio,
                                      h2_fruit_slice_sound_t sound,
                                      uint64_t now_ms) {
  const audio_command_t command = {.at_ms = now_ms, .sound = sound};
  if (h2_pal_queue_send(audio->runtime->queue, audio->worker->commands,
                        &command, 0u) != H2_PAL_OK)
    ++audio->dropped_commands;
}

int h2_fruit_slice_audio_worker_poll(h2_fruit_slice_audio_t* audio) {
  struct h2_fruit_slice_audio_worker* w = audio->worker;
  /* A failed join leaves an intentionally closed producer for the caller to
   * retire on its next mute/lifecycle retry. Do not restart it or turn this
   * pending shutdown into the App's permanent audio-disable error path. */
  if (w->closing) return H2_PAL_OK;
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
  if (!w->closing) {
    int rc = h2_pal_queue_close(audio->runtime->queue, w->commands);
    if (rc != H2_PAL_OK) return rc;
    w->closing = true;
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
  h2_pal_mem_free(audio->runtime->mem, w);
  return H2_PAL_OK;
}
