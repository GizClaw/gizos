#include "h2_lua_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int is_terminal(h2_lua_job_state_t state) {
  return state == H2_LUA_JOB_SUCCEEDED || state == H2_LUA_JOB_FAILED ||
         state == H2_LUA_JOB_CANCELLED || state == H2_LUA_JOB_TIMED_OUT ||
         state == H2_LUA_JOB_STOPPED;
}

static int module_name_is_reserved(const char *name) {
  static const char *const reserved[] = {
      "runtime",   "delay", "system", "display",
      "lcd_touch", "audio", "json",   "capability",
  };
  for (size_t i = 0u; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
    if (strcmp(name, reserved[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static void worker_entry(void *context) {
  h2_lua_worker_t *worker = context;
  h2_lua_host_t *host = worker->host;
  uint8_t wake = 0u;
  while (atomic_load(&host->stopping) == 0) {
    uint64_t sweep_started_ms = h2_lua_now_ms(host);
    int progressed = 0;
    size_t i;
    for (i = worker->index; i < host->config.max_jobs;
         i += host->config.worker_count) {
      h2_lua_job_t *job = &host->jobs[i];
      h2_lua_job_id_t terminal_job_id = H2_LUA_JOB_ID_NONE;
      uint32_t terminal_job_generation = 0u;
      if (h2_pal_mutex_lock(host->config.runtime->sync, job->mutex) !=
          H2_PAL_OK) {
        continue;
      }
      if (job->id != H2_LUA_JOB_ID_NONE && !is_terminal(job->state)) {
        (void)h2_lua_step_job(job);
        if (job->state == H2_LUA_JOB_QUEUED ||
            job->state == H2_LUA_JOB_RUNNING) {
          progressed = 1;
        }
        if (is_terminal(job->state)) {
          terminal_job_id = job->id;
          terminal_job_generation = job->generation;
        }
      }
      (void)h2_pal_mutex_unlock(host->config.runtime->sync, job->mutex);
      h2_lua_cancel_job_capabilities(host, terminal_job_id,
                                     terminal_job_generation);
    }
    if (!progressed && atomic_load(&host->stopping) == 0) {
      (void)h2_pal_queue_recv(host->config.runtime->queue, worker->wake_queue,
                              &wake, 10u);
    } else if (progressed && h2_lua_now_ms(host) - sweep_started_ms >=
                                 host->config.resume_time_budget_ms) {
      (void)h2_pal_time_sleep_ms(host->config.runtime->time, 1u);
    }
  }
}

void h2_lua_host_wake_job(h2_lua_job_t *job) {
  uint8_t wake = 1u;
  if (job == NULL || job->host == NULL || job->host->workers == NULL ||
      job->worker_index >= job->host->config.worker_count) {
    return;
  }
  (void)h2_pal_queue_send_latest(
      job->host->config.runtime->queue,
      job->host->workers[job->worker_index].wake_queue, &wake);
}

void *h2_lua_runtime_realloc(void *user, void *ptr, size_t old_size,
                             size_t new_size) {
  const h2_pal_mem_api_t *mem = (const h2_pal_mem_api_t *)user;
  (void)old_size;
  if (new_size == 0u) {
    h2_pal_mem_free(mem, ptr);
    return NULL;
  }
  if (ptr == NULL) {
    return h2_pal_mem_alloc(mem, new_size);
  }
  return h2_pal_mem_realloc(mem, ptr, new_size);
}

uint64_t h2_lua_now_ms(const h2_lua_host_t *host) {
  uint64_t now = 0u;
  if (host != NULL && host->config.runtime != NULL) {
    (void)h2_pal_time_get_monotonic_ms(host->config.runtime->time, &now);
  }
  return now;
}

static void release_job(h2_lua_job_t *job) {
  const h2_pal_mem_api_t *mem;
  h2_pal_mutex_t *mutex;
  h2_lua_job_id_t job_id;
  uint32_t job_generation;
  size_t i;
  if (job == NULL || job->host == NULL) {
    return;
  }
  mem = job->host->config.runtime->mem;
  mutex = job->mutex;
  job_id = job->id;
  job_generation = job->generation;
  h2_lua_release_job_capabilities(job->host, job_id, job_generation);
  for (i = 0u; i < job->host->config.max_coroutines_per_vm; ++i) {
    h2_lua_task_timer_destroy(&job->tasks[i]);
  }
  if (job->display_open) {
    (void)h2_pal_display_close(job->host->config.runtime->display);
  }
  if (job->touch_open) {
    (void)h2_pal_touch_close(job->host->config.runtime->touch);
  }
  h2_lua_job_close_audio_tracks(job);
  h2_pal_mem_free(mem, job->framebuffer);
  h2_pal_mem_free(mem, job->tasks);
  h2_pal_mem_free(mem, job->callbacks);
  h2_pal_mem_free(mem, job->events);
  h2_pal_mem_free(mem, job->audio_tracks);
  h2_lua_vm_close(job->vm);
  memset(job, 0, sizeof(*job));
  job->mutex = mutex;
}

void h2_lua_cancel_job_capabilities(h2_lua_host_t *host, h2_lua_job_id_t job_id,
                                    uint32_t job_generation) {
  size_t i;
  if (host == NULL || job_id == H2_LUA_JOB_ID_NONE) {
    return;
  }
  for (i = 0u; i < host->config.pending_capability_capacity; ++i) {
    h2_lua_capability_cancel_fn cancel = NULL;
    void *cancel_user = NULL;
    h2_lua_capability_request_id_t request_id = 0u;
    int locked = 0;
    if (host->capability_mutex != NULL) {
      if (h2_pal_mutex_lock(host->config.runtime->sync,
                            host->capability_mutex) != H2_PAL_OK) {
        return;
      }
      locked = 1;
    }
    h2_lua_capability_request_t *request = &host->capability_requests[i];
    if (request->state == H2_LUA_CAPABILITY_REQUEST_PENDING &&
        request->job_id == job_id &&
        request->job_generation == job_generation) {
      request->state = H2_LUA_CAPABILITY_REQUEST_CANCELLED;
      if (request->capability != NULL) {
        cancel = request->capability->cancel;
        cancel_user = request->capability->user;
        request_id = request->id;
      }
    }
    if (locked) {
      (void)h2_pal_mutex_unlock(host->config.runtime->sync,
                                host->capability_mutex);
    }
    if (cancel != NULL) {
      cancel(cancel_user, request_id);
    }
  }
}

void h2_lua_release_job_capabilities(h2_lua_host_t *host,
                                     h2_lua_job_id_t job_id,
                                     uint32_t job_generation) {
  size_t i;
  if (host == NULL || job_id == H2_LUA_JOB_ID_NONE) {
    return;
  }
  for (i = 0u; i < host->config.pending_capability_capacity; ++i) {
    h2_lua_capability_cancel_fn cancel = NULL;
    void *cancel_user = NULL;
    h2_lua_capability_request_id_t request_id = 0u;
    if (host->capability_mutex != NULL &&
        h2_pal_mutex_lock(host->config.runtime->sync, host->capability_mutex) !=
            H2_PAL_OK) {
      return;
    }
    h2_lua_capability_request_t *request = &host->capability_requests[i];
    if (request->state != H2_LUA_CAPABILITY_REQUEST_UNUSED &&
        request->job_id == job_id &&
        request->job_generation == job_generation) {
      if (request->state == H2_LUA_CAPABILITY_REQUEST_PENDING &&
          request->capability != NULL) {
        cancel = request->capability->cancel;
        cancel_user = request->capability->user;
        request_id = request->id;
      }
      memset(request, 0, sizeof(*request));
    }
    if (host->capability_mutex != NULL) {
      (void)h2_pal_mutex_unlock(host->config.runtime->sync,
                                host->capability_mutex);
    }
    if (cancel != NULL) {
      cancel(cancel_user, request_id);
    }
  }
}

h2_pal_result_t h2_lua_host_create(const h2_lua_host_config_t *config,
                                   h2_lua_host_t **out_host) {
  h2_lua_host_t *host;
  h2_lua_host_config_t normalized;
  size_t i;
  if (config == NULL || out_host == NULL || config->runtime == NULL ||
      config->runtime->mem == NULL || config->runtime->time == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_host = NULL;
  if (config->runtime->queue == NULL ||
      config->runtime->queue->vtable == NULL ||
      config->runtime->queue->vtable->create == NULL ||
      config->runtime->queue->vtable->destroy == NULL ||
      config->runtime->queue->vtable->recv == NULL ||
      config->runtime->queue->vtable->close == NULL ||
      (config->runtime->queue->vtable->send_latest == NULL &&
       config->runtime->queue->vtable->send == NULL) ||
      config->runtime->task == NULL || config->runtime->task->vtable == NULL ||
      config->runtime->task->vtable->start == NULL ||
      config->runtime->task->vtable->join == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  normalized = *config;
  normalized.worker_count =
      normalized.worker_count == 0u ? 1u : normalized.worker_count;
  normalized.worker_stack_size = normalized.worker_stack_size == 0u
                                     ? 64u * 1024u
                                     : normalized.worker_stack_size;
  normalized.max_jobs = normalized.max_jobs == 0u ? 4u : normalized.max_jobs;
  normalized.event_delivery_capacity = normalized.event_delivery_capacity == 0u
                                           ? 8u
                                           : normalized.event_delivery_capacity;
  normalized.callback_capacity_per_job =
      normalized.callback_capacity_per_job == 0u
          ? 8u
          : normalized.callback_capacity_per_job;
  normalized.audio_track_capacity_per_job =
      normalized.audio_track_capacity_per_job == 0u
          ? 8u
          : normalized.audio_track_capacity_per_job;
  normalized.max_coroutines_per_vm = normalized.max_coroutines_per_vm == 0u
                                         ? 16u
                                         : normalized.max_coroutines_per_vm;
  normalized.ready_queue_capacity = normalized.ready_queue_capacity == 0u
                                        ? normalized.max_coroutines_per_vm
                                        : normalized.ready_queue_capacity;
  normalized.waiter_capacity = normalized.waiter_capacity == 0u
                                   ? normalized.max_coroutines_per_vm
                                   : normalized.waiter_capacity;
  normalized.pending_capability_capacity =
      normalized.pending_capability_capacity == 0u
          ? 16u
          : normalized.pending_capability_capacity;
  normalized.vm_memory_limit_bytes = normalized.vm_memory_limit_bytes == 0u
                                         ? 256u * 1024u
                                         : normalized.vm_memory_limit_bytes;
  normalized.source_limit_bytes = normalized.source_limit_bytes == 0u
                                      ? 128u * 1024u
                                      : normalized.source_limit_bytes;
  normalized.output_limit_bytes = normalized.output_limit_bytes == 0u
                                      ? 4096u
                                      : normalized.output_limit_bytes;
  normalized.instruction_quantum = normalized.instruction_quantum == 0u
                                       ? 10000u
                                       : normalized.instruction_quantum;
  normalized.resume_time_budget_ms = normalized.resume_time_budget_ms == 0u
                                         ? 5u
                                         : normalized.resume_time_budget_ms;
  normalized.execution_timeout_ms = normalized.execution_timeout_ms == 0u
                                        ? 30000u
                                        : normalized.execution_timeout_ms;
  if (normalized.worker_count > normalized.max_jobs ||
      normalized.ready_queue_capacity < normalized.max_coroutines_per_vm ||
      normalized.waiter_capacity < normalized.max_coroutines_per_vm ||
      normalized.max_jobs > SIZE_MAX / sizeof(h2_lua_job_t) ||
      normalized.worker_count > SIZE_MAX / sizeof(h2_lua_worker_t) ||
      normalized.pending_capability_capacity >
          SIZE_MAX / sizeof(h2_lua_capability_request_t) ||
      normalized.callback_capacity_per_job >
          SIZE_MAX / sizeof(h2_lua_callback_t) ||
      normalized.audio_track_capacity_per_job >
          SIZE_MAX / sizeof(h2_lua_audio_track_slot_t) ||
      normalized.event_delivery_capacity >
          SIZE_MAX / sizeof(h2_lua_event_record_t) ||
      normalized.max_coroutines_per_vm > SIZE_MAX / sizeof(h2_lua_task_t) ||
      normalized.instruction_quantum > INT_MAX ||
      normalized.utc_offset_minutes < -1439 ||
      normalized.utc_offset_minutes > 1439 ||
      (normalized.resource_count != 0u && normalized.resources == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  for (i = 0u; i < normalized.resource_count; ++i) {
    const h2_lua_resource_t *resource = &normalized.resources[i];
    size_t other;
    if (resource->name == NULL || resource->name[0] == '\0' ||
        resource->source == NULL ||
        resource->source_size > normalized.source_limit_bytes ||
        (resource->source_size != 0u && resource->source[0] == 0x1bu)) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    for (other = 0u; other < i; ++other) {
      if (strcmp(normalized.resources[other].name, resource->name) == 0) {
        return H2_PAL_ERR_INVALID_ARG;
      }
    }
  }

  host = h2_pal_mem_alloc(normalized.runtime->mem, sizeof(*host));
  if (host == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(host, 0, sizeof(*host));
  host->config = normalized;
  atomic_init(&host->started, 0);
  atomic_init(&host->stopping, 0);
  atomic_init(&host->joined, 0);
  host->next_job_id = 1u;
  host->next_job_generation = 1u;
  host->next_capability_request_id = 1u;
  host->jobs = h2_pal_mem_alloc(normalized.runtime->mem,
                                normalized.max_jobs * sizeof(*host->jobs));
  if (host->jobs == NULL) {
    h2_pal_mem_free(normalized.runtime->mem, host);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(host->jobs, 0, normalized.max_jobs * sizeof(*host->jobs));
  host->workers =
      h2_pal_mem_alloc(normalized.runtime->mem,
                       normalized.worker_count * sizeof(*host->workers));
  if (host->workers == NULL) {
    h2_pal_mem_free(normalized.runtime->mem, host->jobs);
    h2_pal_mem_free(normalized.runtime->mem, host);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(host->workers, 0, normalized.worker_count * sizeof(*host->workers));
  host->capability_requests = h2_pal_mem_alloc(
      normalized.runtime->mem, normalized.pending_capability_capacity *
                                   sizeof(*host->capability_requests));
  if (host->capability_requests == NULL) {
    h2_pal_mem_free(normalized.runtime->mem, host->workers);
    h2_pal_mem_free(normalized.runtime->mem, host->jobs);
    h2_pal_mem_free(normalized.runtime->mem, host);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(host->capability_requests, 0,
         normalized.pending_capability_capacity *
             sizeof(*host->capability_requests));
  if (normalized.runtime->sync != NULL) {
    h2_pal_result_t mutex_result =
        h2_pal_mutex_create(normalized.runtime->sync,
                            &(h2_pal_mutex_config_t){
                                .name = "h2-lua-capability",
                                .allocator = normalized.runtime->mem,
                            },
                            &host->capability_mutex);
    if (mutex_result != H2_PAL_OK && mutex_result != H2_PAL_ERR_UNSUPPORTED) {
      h2_pal_mem_free(normalized.runtime->mem, host->capability_requests);
      h2_pal_mem_free(normalized.runtime->mem, host->workers);
      h2_pal_mem_free(normalized.runtime->mem, host->jobs);
      h2_pal_mem_free(normalized.runtime->mem, host);
      return mutex_result;
    }
    mutex_result = h2_pal_mutex_create(normalized.runtime->sync,
                                       &(h2_pal_mutex_config_t){
                                           .name = "h2-lua-jobs",
                                           .allocator = normalized.runtime->mem,
                                       },
                                       &host->jobs_mutex);
    if (mutex_result != H2_PAL_OK) {
      if (host->capability_mutex != NULL) {
        (void)h2_pal_mutex_destroy(normalized.runtime->sync,
                                   host->capability_mutex);
      }
      h2_pal_mem_free(normalized.runtime->mem, host->capability_requests);
      h2_pal_mem_free(normalized.runtime->mem, host->workers);
      h2_pal_mem_free(normalized.runtime->mem, host->jobs);
      h2_pal_mem_free(normalized.runtime->mem, host);
      return mutex_result;
    }
    mutex_result = h2_pal_mutex_create(normalized.runtime->sync,
                                       &(h2_pal_mutex_config_t){
                                           .name = "h2-lua-audio",
                                           .allocator = normalized.runtime->mem,
                                       },
                                       &host->audio_mutex);
    if (mutex_result != H2_PAL_OK) {
      (void)h2_pal_mutex_destroy(normalized.runtime->sync, host->jobs_mutex);
      if (host->capability_mutex != NULL) {
        (void)h2_pal_mutex_destroy(normalized.runtime->sync,
                                   host->capability_mutex);
      }
      h2_pal_mem_free(normalized.runtime->mem, host->capability_requests);
      h2_pal_mem_free(normalized.runtime->mem, host->workers);
      h2_pal_mem_free(normalized.runtime->mem, host->jobs);
      h2_pal_mem_free(normalized.runtime->mem, host);
      return mutex_result;
    }
    for (size_t job_index = 0u; job_index < normalized.max_jobs; ++job_index) {
      mutex_result =
          h2_pal_mutex_create(normalized.runtime->sync,
                              &(h2_pal_mutex_config_t){
                                  .name = "h2-lua-job",
                                  .allocator = normalized.runtime->mem,
                              },
                              &host->jobs[job_index].mutex);
      if (mutex_result != H2_PAL_OK) {
        for (size_t created_index = 0u; created_index < job_index;
             ++created_index) {
          (void)h2_pal_mutex_destroy(normalized.runtime->sync,
                                     host->jobs[created_index].mutex);
        }
        (void)h2_pal_mutex_destroy(normalized.runtime->sync, host->audio_mutex);
        (void)h2_pal_mutex_destroy(normalized.runtime->sync, host->jobs_mutex);
        if (host->capability_mutex != NULL) {
          (void)h2_pal_mutex_destroy(normalized.runtime->sync,
                                     host->capability_mutex);
        }
        h2_pal_mem_free(normalized.runtime->mem, host->capability_requests);
        h2_pal_mem_free(normalized.runtime->mem, host->workers);
        h2_pal_mem_free(normalized.runtime->mem, host->jobs);
        h2_pal_mem_free(normalized.runtime->mem, host);
        return mutex_result;
      }
    }
  } else {
    h2_pal_mem_free(normalized.runtime->mem, host->capability_requests);
    h2_pal_mem_free(normalized.runtime->mem, host->workers);
    h2_pal_mem_free(normalized.runtime->mem, host->jobs);
    h2_pal_mem_free(normalized.runtime->mem, host);
    return H2_PAL_ERR_UNSUPPORTED;
  }
  *out_host = host;
  return H2_PAL_OK;
}

h2_pal_result_t h2_lua_host_start(h2_lua_host_t *host) {
  if (host == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (atomic_load(&host->started) != 0) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (atomic_load(&host->stopping) != 0) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  for (size_t i = 0u; i < host->config.worker_count; ++i) {
    h2_pal_result_t result;
    host->workers[i].host = host;
    host->workers[i].index = i;
    result = (h2_pal_result_t)h2_pal_queue_create(
        host->config.runtime->queue,
        &(h2_pal_queue_config_t){
            .name = "h2-lua-worker",
            .item_size = sizeof(uint8_t),
            .item_count = 1u,
            .allocator = host->config.runtime->mem,
        },
        &host->workers[i].wake_queue);
    if (result != H2_PAL_OK) {
      atomic_store(&host->stopping, 1);
      (void)h2_lua_host_join(host);
      return result;
    }
    result = h2_pal_task_start(
        host->config.runtime->task,
        &(h2_pal_task_options_t){
            .name = "h2-lua-worker",
            .min_stack_size = host->config.worker_stack_size,
        },
        worker_entry, &host->workers[i], &host->workers[i].task);
    if (result != H2_PAL_OK) {
      atomic_store(&host->stopping, 1);
      (void)h2_lua_host_join(host);
      return result;
    }
  }
  atomic_store(&host->started, 1);
  return H2_PAL_OK;
}

h2_pal_result_t h2_lua_host_stop(h2_lua_host_t *host) {
  size_t i;
  if (host == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  atomic_store(&host->stopping, 1);
  for (i = 0u; i < host->config.max_jobs; ++i) {
    h2_lua_job_id_t job_id = H2_LUA_JOB_ID_NONE;
    uint32_t job_generation = 0u;
    if (h2_pal_mutex_lock(host->config.runtime->sync, host->jobs[i].mutex) !=
        H2_PAL_OK) {
      continue;
    }
    if (host->jobs[i].id != H2_LUA_JOB_ID_NONE &&
        !is_terminal(host->jobs[i].state)) {
      job_id = host->jobs[i].id;
      job_generation = host->jobs[i].generation;
      for (size_t task_index = 0u;
           task_index < host->config.max_coroutines_per_vm; ++task_index) {
        h2_lua_task_timer_destroy(&host->jobs[i].tasks[task_index]);
      }
      h2_lua_job_finish(&host->jobs[i], H2_LUA_JOB_STOPPED, "host stopped");
    }
    (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs[i].mutex);
    h2_lua_cancel_job_capabilities(host, job_id, job_generation);
  }
  for (i = 0u; i < host->config.worker_count; ++i) {
    if (host->workers[i].wake_queue != NULL) {
      (void)h2_pal_queue_close(host->config.runtime->queue,
                               host->workers[i].wake_queue);
    }
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_lua_host_join(h2_lua_host_t *host) {
  h2_pal_result_t result = H2_PAL_OK;
  if (host == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (atomic_load(&host->joined) != 0) {
    return H2_PAL_OK;
  }
  for (size_t i = 0u; i < host->config.worker_count; ++i) {
    if (host->workers[i].task != NULL) {
      h2_pal_result_t join_result =
          h2_pal_task_join(host->config.runtime->task, host->workers[i].task);
      if (join_result == H2_PAL_OK) {
        host->workers[i].task = NULL;
      } else if (result == H2_PAL_OK) {
        result = join_result;
      }
    }
    if (host->workers[i].wake_queue != NULL && host->workers[i].task == NULL) {
      h2_pal_queue_destroy(host->config.runtime->queue,
                           host->workers[i].wake_queue);
      host->workers[i].wake_queue = NULL;
    }
  }
  if (result == H2_PAL_OK) {
    atomic_store(&host->joined, 1);
  }
  return result;
}

void h2_lua_host_destroy(h2_lua_host_t *host) {
  size_t i;
  const h2_pal_mem_api_t *mem;
  if (host == NULL) {
    return;
  }
  mem = host->config.runtime->mem;
  (void)h2_lua_host_stop(host);
  if (h2_lua_host_join(host) != H2_PAL_OK) {
    return;
  }
  for (i = 0u; i < host->config.max_jobs; ++i) {
    release_job(&host->jobs[i]);
  }
  if (host->capability_mutex != NULL) {
    (void)h2_pal_mutex_destroy(host->config.runtime->sync,
                               host->capability_mutex);
  }
  if (host->audio_mutex != NULL) {
    (void)h2_pal_mutex_destroy(host->config.runtime->sync, host->audio_mutex);
  }
  if (host->jobs_mutex != NULL) {
    (void)h2_pal_mutex_destroy(host->config.runtime->sync, host->jobs_mutex);
  }
  for (i = 0u; i < host->config.max_jobs; ++i) {
    if (host->jobs[i].mutex != NULL) {
      (void)h2_pal_mutex_destroy(host->config.runtime->sync,
                                 host->jobs[i].mutex);
    }
  }
  h2_pal_mem_free(mem, host->jobs);
  h2_pal_mem_free(mem, host->workers);
  h2_pal_mem_free(mem, host->capability_requests);
  h2_pal_mem_free(mem, host);
}

h2_pal_result_t h2_lua_register_module(h2_lua_host_t *host, const char *name,
                                       h2_lua_module_open_fn open_fn,
                                       void *user) {
  h2_lua_module_entry_t *entry;
  if (host == NULL || name == NULL || open_fn == NULL || name[0] == '\0' ||
      strlen(name) >= H2_LUA_NAME_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (atomic_load(&host->started) != 0) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (module_name_is_reserved(name)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  for (size_t i = 0u; i < host->module_count; ++i) {
    if (strcmp(host->modules[i].name, name) == 0) {
      return H2_PAL_ERR_INVALID_STATE;
    }
  }
  if (host->module_count == sizeof(host->modules) / sizeof(host->modules[0])) {
    return H2_PAL_ERR_FULL;
  }
  entry = &host->modules[host->module_count++];
  (void)strcpy(entry->name, name);
  entry->open_fn = open_fn;
  entry->user = user;
  return H2_PAL_OK;
}

h2_pal_result_t h2_lua_register_capability(h2_lua_host_t *host,
                                           const char *name,
                                           h2_lua_capability_call_fn call,
                                           h2_lua_capability_cancel_fn cancel,
                                           void *user) {
  h2_lua_capability_entry_t *entry;
  if (host == NULL || name == NULL || call == NULL || name[0] == '\0' ||
      strlen(name) >= H2_LUA_NAME_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (atomic_load(&host->started) != 0) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  for (size_t i = 0u; i < host->capability_count; ++i) {
    if (strcmp(host->capabilities[i].name, name) == 0) {
      return H2_PAL_ERR_INVALID_STATE;
    }
  }
  if (host->capability_count ==
      sizeof(host->capabilities) / sizeof(host->capabilities[0])) {
    return H2_PAL_ERR_FULL;
  }
  entry = &host->capabilities[host->capability_count++];
  (void)strcpy(entry->name, name);
  entry->call = call;
  entry->cancel = cancel;
  entry->user = user;
  return H2_PAL_OK;
}

h2_lua_capability_request_t *
h2_lua_find_capability_request(h2_lua_host_t *host,
                               h2_lua_capability_request_id_t request_id) {
  size_t i;
  if (host == NULL || request_id == 0u) {
    return NULL;
  }
  for (i = 0u; i < host->config.pending_capability_capacity; ++i) {
    if (host->capability_requests[i].state !=
            H2_LUA_CAPABILITY_REQUEST_UNUSED &&
        host->capability_requests[i].id == request_id) {
      return &host->capability_requests[i];
    }
  }
  return NULL;
}

h2_pal_result_t h2_lua_capability_complete(
    h2_lua_host_t *host, h2_lua_capability_request_id_t request_id,
    h2_pal_result_t result, const char *output, const char *error) {
  h2_lua_capability_request_t *request;
  h2_pal_result_t lock_result;
  if (host == NULL || request_id == 0u || result == H2_PAL_ERR_WOULD_BLOCK ||
      (result == H2_PAL_OK && output == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if ((output != NULL && strlen(output) >= H2_LUA_CAPABILITY_OUTPUT_MAX) ||
      (error != NULL && strlen(error) >= H2_LUA_MESSAGE_MAX)) {
    return H2_PAL_ERR_NO_SPACE;
  }
  if (host->capability_mutex == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  lock_result =
      h2_pal_mutex_lock(host->config.runtime->sync, host->capability_mutex);
  if (lock_result != H2_PAL_OK) {
    return lock_result;
  }
  request = h2_lua_find_capability_request(host, request_id);
  if (request == NULL) {
    lock_result = H2_PAL_ERR_NOT_FOUND;
  } else if (request->state == H2_LUA_CAPABILITY_REQUEST_CANCELLED) {
    lock_result = H2_PAL_ERR_CLOSED;
  } else if (request->state != H2_LUA_CAPABILITY_REQUEST_PENDING) {
    lock_result = H2_PAL_ERR_INVALID_STATE;
  } else {
    request->result = result;
    (void)snprintf(request->output, sizeof(request->output), "%s",
                   output == NULL ? "" : output);
    (void)snprintf(request->error, sizeof(request->error), "%s",
                   error == NULL ? "capability failed" : error);
    request->state = H2_LUA_CAPABILITY_REQUEST_COMPLETED;
    lock_result = H2_PAL_OK;
  }
  (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->capability_mutex);
  if (lock_result == H2_PAL_OK) {
    uint8_t wake = 1u;
    for (size_t i = 0u; i < host->config.worker_count; ++i) {
      (void)h2_pal_queue_send_latest(host->config.runtime->queue,
                                     host->workers[i].wake_queue, &wake);
    }
  }
  return lock_result;
}
