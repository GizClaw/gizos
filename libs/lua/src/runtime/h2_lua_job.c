#include "h2_lua_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

h2_pal_result_t h2_lua_job_acquire_audio_speaker(h2_lua_job_t *job) {
  h2_lua_host_t *host;
  h2_pal_result_t result;
  if (job == NULL || job->host == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (job->audio_speaker_acquired) {
    return H2_PAL_OK;
  }
  host = job->host;
  result = h2_pal_mutex_lock(host->config.runtime->sync, host->audio_mutex);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (host->audio_speaker_users == 0u) {
    result = h2_pal_audio_start_speaker(host->config.runtime->audio);
  }
  if (result == H2_PAL_OK) {
    host->audio_speaker_users++;
    job->audio_speaker_acquired = 1;
  }
  (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->audio_mutex);
  return result;
}

void h2_lua_job_release_audio_speaker(h2_lua_job_t *job) {
  h2_lua_host_t *host;
  if (job == NULL || job->host == NULL || !job->audio_speaker_acquired) {
    return;
  }
  host = job->host;
  if (h2_pal_mutex_lock(host->config.runtime->sync, host->audio_mutex) !=
      H2_PAL_OK) {
    return;
  }
  job->audio_speaker_acquired = 0;
  if (host->audio_speaker_users != 0u) {
    host->audio_speaker_users--;
  }
  if (host->audio_speaker_users == 0u) {
    (void)h2_pal_audio_stop_speaker(host->config.runtime->audio);
  }
  (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->audio_mutex);
}

h2_pal_result_t h2_lua_job_acquire_audio_mic(h2_lua_job_t *job) {
  h2_lua_host_t *host;
  h2_pal_result_t result;
  if (job == NULL || job->host == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (job->audio_mic_acquired) {
    return H2_PAL_OK;
  }
  host = job->host;
  result = h2_pal_mutex_lock(host->config.runtime->sync, host->audio_mutex);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (host->audio_mic_users == 0u) {
    result = h2_pal_audio_start_mic(host->config.runtime->audio);
  }
  if (result == H2_PAL_OK) {
    host->audio_mic_users++;
    job->audio_mic_acquired = 1;
  }
  (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->audio_mutex);
  return result;
}

void h2_lua_job_release_audio_mic(h2_lua_job_t *job) {
  h2_lua_host_t *host;
  if (job == NULL || job->host == NULL || !job->audio_mic_acquired) {
    return;
  }
  host = job->host;
  if (h2_pal_mutex_lock(host->config.runtime->sync, host->audio_mutex) !=
      H2_PAL_OK) {
    return;
  }
  job->audio_mic_acquired = 0;
  if (host->audio_mic_users != 0u) {
    host->audio_mic_users--;
  }
  if (host->audio_mic_users == 0u) {
    (void)h2_pal_audio_stop_mic(host->config.runtime->audio);
  }
  (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->audio_mutex);
}

void h2_lua_job_close_audio_tracks(h2_lua_job_t *job) {
  size_t i;
  if (job == NULL || job->host == NULL) {
    return;
  }
  for (i = 0u; i < job->host->config.audio_track_capacity_per_job; ++i) {
    if (job->audio_tracks != NULL && job->audio_tracks[i].track != NULL) {
      h2_lua_audio_track_slot_flush_carry(&job->audio_tracks[i]);
      (void)h2_pal_audio_track_close(job->audio_tracks[i].track);
      job->audio_tracks[i].track = NULL;
    }
    if (job->audio_tracks != NULL) {
      h2_lua_audio_track_slot_release_carry(&job->audio_tracks[i],
                                            job->host->config.runtime->mem);
    }
  }
  job->active_audio_track_count = 0u;
  h2_lua_job_release_audio_speaker(job);
  h2_lua_job_release_audio_mic(job);
  h2_pal_mem_free(job->host->config.runtime->mem, job->audio_mic_buffer);
  job->audio_mic_buffer = NULL;
  job->audio_mic_buffer_capacity = 0u;
  memset(&job->audio_mic_format, 0, sizeof(job->audio_mic_format));
}

static int is_terminal(h2_lua_job_state_t state) {
  return state == H2_LUA_JOB_SUCCEEDED || state == H2_LUA_JOB_FAILED ||
         state == H2_LUA_JOB_CANCELLED || state == H2_LUA_JOB_TIMED_OUT ||
         state == H2_LUA_JOB_STOPPED;
}

void h2_lua_task_timer_destroy(h2_lua_task_t *task) {
  if (task == NULL || task->timer == NULL || task->job == NULL) {
    return;
  }
  (void)h2_pal_timer_stop(task->job->host->config.runtime->timer, task->timer);
  (void)h2_pal_timer_destroy(task->job->host->config.runtime->timer,
                             task->timer);
  task->timer = NULL;
  atomic_store(&task->timer_fired, 0);
}

h2_lua_job_t *h2_lua_find_job(h2_lua_host_t *host, h2_lua_job_id_t id) {
  size_t i;
  if (host == NULL || id == H2_LUA_JOB_ID_NONE) {
    return NULL;
  }
  for (i = 0u; i < host->config.max_jobs; ++i) {
    if (host->jobs[i].id == id) {
      return &host->jobs[i];
    }
  }
  return NULL;
}

h2_pal_result_t h2_lua_lock_job(h2_lua_host_t *host, h2_lua_job_id_t id,
                                h2_lua_job_t **out_job) {
  h2_lua_job_t *job;
  h2_pal_result_t result;
  if (host == NULL || out_job == NULL || id == H2_LUA_JOB_ID_NONE) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_job = NULL;
  result = h2_pal_mutex_lock(host->config.runtime->sync, host->jobs_mutex);
  if (result != H2_PAL_OK) {
    return result;
  }
  job = h2_lua_find_job(host, id);
  if (job == NULL) {
    (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs_mutex);
    return H2_PAL_ERR_NOT_FOUND;
  }
  result = h2_pal_mutex_lock(host->config.runtime->sync, job->mutex);
  (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs_mutex);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (job->id != id) {
    (void)h2_pal_mutex_unlock(host->config.runtime->sync, job->mutex);
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_job = job;
  return H2_PAL_OK;
}

void h2_lua_unlock_job(h2_lua_job_t *job) {
  if (job != NULL && job->host != NULL) {
    (void)h2_pal_mutex_unlock(job->host->config.runtime->sync, job->mutex);
  }
}

void h2_lua_job_finish(h2_lua_job_t *job, h2_lua_job_state_t state,
                       const char *message) {
  if (job == NULL || is_terminal(job->state)) {
    return;
  }
  job->state = state;
  if (message != NULL) {
    (void)snprintf(job->message, sizeof(job->message), "%s", message);
  }
}

static void instruction_hook(lua_State *state, lua_Debug *debug) {
  h2_lua_execution_context_t *context =
      *(h2_lua_execution_context_t **)lua_getextraspace(state);
  h2_lua_task_t *task = context == NULL ? NULL : context->task;
  h2_lua_job_t *job = context == NULL ? NULL : context->job;
  uint64_t now;
  (void)debug;
  if (job == NULL) {
    return;
  }
  if (job->cancel_requested || (task != NULL && task->cancel_requested) ||
      atomic_load(&job->host->stopping) != 0) {
    luaL_error(state, "job cancelled");
  }
  now = h2_lua_now_ms(job->host);
  if (now - job->started_ms >= job->host->config.execution_timeout_ms) {
    luaL_error(state, "job timed out");
  }
  if (task != NULL && now - task->resume_started_ms >=
                          job->host->config.resume_time_budget_ms) {
    task->state = H2_LUA_TASK_READY;
    (void)lua_yield(state, 0);
  }
}

h2_lua_task_t *h2_lua_current_task(lua_State *state) {
  h2_lua_execution_context_t *context;
  if (state == NULL) {
    return NULL;
  }
  context = *(h2_lua_execution_context_t **)lua_getextraspace(state);
  return context == NULL ? NULL : context->task;
}

h2_lua_task_t *h2_lua_find_task(h2_lua_job_t *job, uint32_t task_id) {
  size_t i;
  if (job == NULL || task_id == 0u) {
    return NULL;
  }
  for (i = 0u; i < job->host->config.max_coroutines_per_vm; ++i) {
    if (job->tasks[i].state != H2_LUA_TASK_UNUSED &&
        job->tasks[i].id == task_id) {
      return &job->tasks[i];
    }
  }
  return NULL;
}

static h2_lua_task_t *find_empty_task(h2_lua_job_t *job) {
  size_t i;
  for (i = 0u; i < job->host->config.max_coroutines_per_vm; ++i) {
    if (job->tasks[i].state == H2_LUA_TASK_UNUSED) {
      return &job->tasks[i];
    }
  }
  return NULL;
}

h2_pal_result_t h2_lua_spawn_task(h2_lua_job_t *job, lua_State *source_state,
                                  int function_index, int argument_count,
                                  uint32_t *out_task_id) {
  h2_lua_task_t *task;
  lua_State *root;
  int i;
  if (job == NULL || source_state == NULL || out_task_id == NULL ||
      argument_count < 0 || !lua_isfunction(source_state, function_index)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  task = find_empty_task(job);
  if (task == NULL) {
    return H2_PAL_ERR_FULL;
  }
  root = job->vm->state;
  memset(task, 0, sizeof(*task));
  task->context.job = job;
  task->context.task = task;
  task->job = job;
  task->id = job->next_task_id++;
  if (job->next_task_id == 0u) {
    job->next_task_id = 1u;
  }
  task->state = H2_LUA_TASK_READY;
  task->thread_ref = LUA_NOREF;
  atomic_init(&task->timer_fired, 0);
  task->thread = lua_newthread(root);
  task->thread_ref = luaL_ref(root, LUA_REGISTRYINDEX);
  *(h2_lua_execution_context_t **)lua_getextraspace(task->thread) =
      &task->context;
  lua_pushvalue(source_state, function_index);
  for (i = 0; i < argument_count; ++i) {
    lua_pushvalue(source_state, function_index + 1 + i);
  }
  lua_xmove(source_state, task->thread, argument_count + 1);
  task->resume_argument_count = argument_count;
  lua_sethook(task->thread, instruction_hook, LUA_MASKCOUNT,
              (int)job->host->config.instruction_quantum);
  job->task_count++;
  *out_task_id = task->id;
  return H2_PAL_OK;
}

static h2_lua_job_t *find_empty_job(h2_lua_host_t *host) {
  size_t i;
  for (i = 0u; i < host->config.max_jobs; ++i) {
    if (host->jobs[i].id == H2_LUA_JOB_ID_NONE) {
      return &host->jobs[i];
    }
  }
  return NULL;
}

h2_pal_result_t
h2_lua_job_submit_text(h2_lua_host_t *host, const char *chunk_name,
                       const uint8_t *source, size_t source_size,
                       const h2_lua_arg_t *args, size_t arg_count,
                       h2_lua_job_id_t *out_job_id) {
  h2_lua_job_t *job;
  h2_lua_vm_config_t vm_config;
  lua_State *root;
  h2_lua_job_id_t new_job_id;
  size_t i;
  int load_status;
  if (host == NULL || chunk_name == NULL || source == NULL ||
      out_job_id == NULL || arg_count > INT_MAX ||
      (arg_count != 0u && args == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_job_id = H2_LUA_JOB_ID_NONE;
  if (atomic_load(&host->started) == 0 || atomic_load(&host->stopping) != 0) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (source_size > host->config.source_limit_bytes) {
    return H2_PAL_ERR_NO_SPACE;
  }
  if (memchr(source, '\0', source_size) != NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (source_size != 0u && source[0] == 0x1bu) {
    return H2_PAL_ERR_FORMAT;
  }
  if (h2_pal_mutex_lock(host->config.runtime->sync, host->jobs_mutex) !=
      H2_PAL_OK) {
    return H2_PAL_ERR_BUSY;
  }
  job = find_empty_job(host);
  if (job == NULL) {
    (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs_mutex);
    return H2_PAL_ERR_FULL;
  }
  if (h2_pal_mutex_lock(host->config.runtime->sync, job->mutex) != H2_PAL_OK) {
    (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs_mutex);
    return H2_PAL_ERR_BUSY;
  }
  h2_pal_mutex_t *job_mutex = job->mutex;
  size_t job_index = (size_t)(job - host->jobs);
  memset(job, 0, sizeof(*job));
  job->mutex = job_mutex;
  job->host = host;
  if (chunk_name[0] == '@') {
    const char *slash = strrchr(chunk_name + 1, '/');
    if (slash != NULL) {
      size_t root_length = (size_t)(slash - (chunk_name + 1));
      if (root_length >= sizeof(job->require_root)) {
        (void)h2_pal_mutex_unlock(host->config.runtime->sync, job->mutex);
        (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs_mutex);
        return H2_PAL_ERR_NO_SPACE;
      }
      memcpy(job->require_root, chunk_name + 1, root_length);
      job->require_root[root_length] = '\0';
    }
  }
  new_job_id = host->next_job_id++;
  if (host->next_job_id == H2_LUA_JOB_ID_NONE) {
    host->next_job_id = 1u;
  }
  job->generation = host->next_job_generation++;
  job->worker_index = job_index % host->config.worker_count;
  if (host->next_job_generation == 0u) {
    host->next_job_generation = 1u;
  }
  job->state = H2_LUA_JOB_QUEUED;
  job->next_task_id = 1u;
  job->next_callback_token = 1u;
  job->callbacks = h2_pal_mem_alloc(host->config.runtime->mem,
                                    host->config.callback_capacity_per_job *
                                        sizeof(*job->callbacks));
  job->events = h2_pal_mem_alloc(host->config.runtime->mem,
                                 host->config.event_delivery_capacity *
                                     sizeof(*job->events));
  job->tasks = h2_pal_mem_alloc(host->config.runtime->mem,
                                host->config.max_coroutines_per_vm *
                                    sizeof(*job->tasks));
  job->audio_tracks = h2_pal_mem_alloc(
      host->config.runtime->mem,
      host->config.audio_track_capacity_per_job * sizeof(*job->audio_tracks));
  if (job->callbacks == NULL || job->events == NULL || job->tasks == NULL ||
      job->audio_tracks == NULL) {
    h2_pal_mem_free(host->config.runtime->mem, job->callbacks);
    h2_pal_mem_free(host->config.runtime->mem, job->events);
    h2_pal_mem_free(host->config.runtime->mem, job->tasks);
    h2_pal_mem_free(host->config.runtime->mem, job->audio_tracks);
    memset(job, 0, sizeof(*job));
    job->mutex = job_mutex;
    (void)h2_pal_mutex_unlock(host->config.runtime->sync, job->mutex);
    (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs_mutex);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memset(job->callbacks, 0,
         host->config.callback_capacity_per_job * sizeof(*job->callbacks));
  memset(job->events, 0,
         host->config.event_delivery_capacity * sizeof(*job->events));
  memset(job->tasks, 0,
         host->config.max_coroutines_per_vm * sizeof(*job->tasks));
  memset(job->audio_tracks, 0,
         host->config.audio_track_capacity_per_job *
             sizeof(*job->audio_tracks));
  job->next_audio_track_generation = 1u;
  job->audio_mic_generation = 1u;
  vm_config = (h2_lua_vm_config_t){
      .realloc_fn = h2_lua_runtime_realloc,
      .allocator_user = (void *)host->config.runtime->mem,
      .memory_limit_bytes = host->config.vm_memory_limit_bytes,
      .source_limit_bytes = host->config.source_limit_bytes,
      .output_limit_bytes = host->config.output_limit_bytes,
  };
  if (h2_lua_vm_create(&vm_config, &job->vm) != H2_LUA_VM_OK) {
    h2_pal_mem_free(host->config.runtime->mem, job->callbacks);
    h2_pal_mem_free(host->config.runtime->mem, job->events);
    h2_pal_mem_free(host->config.runtime->mem, job->tasks);
    h2_pal_mem_free(host->config.runtime->mem, job->audio_tracks);
    memset(job, 0, sizeof(*job));
    job->mutex = job_mutex;
    (void)h2_pal_mutex_unlock(host->config.runtime->sync, job->mutex);
    (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs_mutex);
    return H2_PAL_ERR_NO_MEMORY;
  }
  root = job->vm->state;
  job->root_context.job = job;
  *(h2_lua_execution_context_t **)lua_getextraspace(root) = &job->root_context;
  if (h2_lua_register_builtin_modules(job) != H2_PAL_OK) {
    h2_lua_vm_close(job->vm);
    h2_pal_mem_free(host->config.runtime->mem, job->callbacks);
    h2_pal_mem_free(host->config.runtime->mem, job->events);
    h2_pal_mem_free(host->config.runtime->mem, job->tasks);
    h2_pal_mem_free(host->config.runtime->mem, job->audio_tracks);
    memset(job, 0, sizeof(*job));
    job->mutex = job_mutex;
    (void)h2_pal_mutex_unlock(host->config.runtime->sync, job->mutex);
    (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs_mutex);
    return H2_PAL_ERR_INVALID_STATE;
  }
  lua_createtable(root, 0, (int)arg_count);
  for (i = 0u; i < arg_count; ++i) {
    if (args[i].name == NULL || args[i].value == NULL) {
      lua_pop(root, 1);
      h2_lua_vm_close(job->vm);
      h2_pal_mem_free(host->config.runtime->mem, job->callbacks);
      h2_pal_mem_free(host->config.runtime->mem, job->events);
      h2_pal_mem_free(host->config.runtime->mem, job->tasks);
      h2_pal_mem_free(host->config.runtime->mem, job->audio_tracks);
      memset(job, 0, sizeof(*job));
      job->mutex = job_mutex;
      (void)h2_pal_mutex_unlock(host->config.runtime->sync, job->mutex);
      (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs_mutex);
      return H2_PAL_ERR_INVALID_ARG;
    }
    lua_pushstring(root, args[i].value);
    lua_setfield(root, -2, args[i].name);
  }
  lua_setglobal(root, "args");
  job->tasks[0].job = job;
  job->tasks[0].context.job = job;
  job->tasks[0].context.task = &job->tasks[0];
  job->tasks[0].id = job->next_task_id++;
  job->tasks[0].state = H2_LUA_TASK_READY;
  job->tasks[0].thread_ref = LUA_NOREF;
  atomic_init(&job->tasks[0].timer_fired, 0);
  job->tasks[0].thread = lua_newthread(root);
  job->tasks[0].thread_ref = luaL_ref(root, LUA_REGISTRYINDEX);
  *(h2_lua_execution_context_t **)lua_getextraspace(job->tasks[0].thread) =
      &job->tasks[0].context;
  job->task_count = 1u;
  load_status = luaL_loadbufferx(job->tasks[0].thread, (const char *)source,
                                 source_size, chunk_name, "t");
  if (load_status != LUA_OK) {
    h2_lua_job_finish(job, H2_LUA_JOB_FAILED,
                      lua_tostring(job->tasks[0].thread, -1));
  }
  job->started_ms = h2_lua_now_ms(host);
  lua_sethook(job->tasks[0].thread, instruction_hook, LUA_MASKCOUNT,
              (int)host->config.instruction_quantum);
  job->id = new_job_id;
  *out_job_id = new_job_id;
  (void)h2_pal_mutex_unlock(host->config.runtime->sync, job->mutex);
  (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs_mutex);
  h2_lua_host_wake_job(job);
  return H2_PAL_OK;
}

static int relative_path_is_valid(const char *path) {
  const char *segment;
  const char *cursor;
  if (path == NULL || path[0] == '\0' || path[0] == '/' || path[0] == '\\' ||
      strlen(path) >= H2_LUA_PATH_MAX || strchr(path, '\\') != NULL ||
      strchr(path, ':') != NULL) {
    return 0;
  }
  segment = path;
  for (cursor = path;; ++cursor) {
    if (*cursor == '/' || *cursor == '\0') {
      size_t length = (size_t)(cursor - segment);
      if (length == 0u || (length == 1u && segment[0] == '.') ||
          (length == 2u && segment[0] == '.' && segment[1] == '.')) {
        return 0;
      }
      if (*cursor == '\0') {
        break;
      }
      segment = cursor + 1;
    }
  }
  return 1;
}

h2_pal_result_t h2_lua_job_submit_resource(h2_lua_host_t *host,
                                           const char *resource_name,
                                           const h2_lua_arg_t *args,
                                           size_t arg_count,
                                           h2_lua_job_id_t *out_job_id) {
  size_t i;
  if (host == NULL || resource_name == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  for (i = 0u; i < host->config.resource_count; ++i) {
    const h2_lua_resource_t *resource = &host->config.resources[i];
    if (resource->name != NULL && strcmp(resource->name, resource_name) == 0) {
      if (resource->source == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
      }
      return h2_lua_job_submit_text(host, resource->name, resource->source,
                                    resource->source_size, args, arg_count,
                                    out_job_id);
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}

h2_pal_result_t h2_lua_job_submit_file(h2_lua_host_t *host,
                                       const char *relative_path,
                                       const h2_lua_arg_t *args,
                                       size_t arg_count,
                                       h2_lua_job_id_t *out_job_id) {
  h2_pal_fs_stat_t stat;
  h2_pal_fs_file_t *file = NULL;
  uint8_t *source = NULL;
  size_t offset = 0u;
  h2_pal_result_t result;
  char chunk_name[H2_LUA_PATH_MAX + 2u];
  if (host == NULL || out_job_id == NULL ||
      !relative_path_is_valid(relative_path)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (host->config.runtime->fs == NULL ||
      host->config.runtime->fs->vtable == NULL ||
      host->config.runtime->fs->vtable->stat == NULL ||
      host->config.runtime->fs->vtable->open == NULL ||
      host->config.runtime->fs->vtable->read == NULL ||
      host->config.runtime->fs->vtable->close == NULL) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  result = (h2_pal_result_t)h2_pal_fs_stat(host->config.runtime->fs,
                                           relative_path, &stat);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (stat.is_dir || stat.size > host->config.source_limit_bytes ||
      stat.size > SIZE_MAX - 1u) {
    return stat.is_dir ? H2_PAL_ERR_FORMAT : H2_PAL_ERR_NO_SPACE;
  }
  source = h2_pal_mem_alloc(host->config.runtime->mem, (size_t)stat.size + 1u);
  if (source == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  result = (h2_pal_result_t)h2_pal_fs_open(
      host->config.runtime->fs, relative_path, H2_PAL_FS_OPEN_READ, &file);
  while (result == H2_PAL_OK && offset < (size_t)stat.size) {
    size_t read_size = 0u;
    result = (h2_pal_result_t)h2_pal_fs_read(
        host->config.runtime->fs, file, source + offset,
        (size_t)stat.size - offset, &read_size);
    if (result == H2_PAL_OK && read_size == 0u) {
      result = H2_PAL_ERR_TRUNCATED;
    }
    offset += read_size;
  }
  if (file != NULL) {
    h2_pal_result_t close_result =
        (h2_pal_result_t)h2_pal_fs_close(host->config.runtime->fs, file);
    if (result == H2_PAL_OK) {
      result = close_result;
    }
  }
  if (result == H2_PAL_OK) {
    source[offset] = '\0';
    (void)snprintf(chunk_name, sizeof(chunk_name), "@%s", relative_path);
    result = h2_lua_job_submit_text(host, chunk_name, source, offset, args,
                                    arg_count, out_job_id);
  }
  h2_pal_mem_free(host->config.runtime->mem, source);
  return result;
}

static int task_is_terminal(h2_lua_task_state_t state) {
  return state == H2_LUA_TASK_DONE || state == H2_LUA_TASK_FAILED ||
         state == H2_LUA_TASK_CANCELLED;
}

static void update_waiters(h2_lua_job_t *job, uint64_t now) {
  size_t i;
  int capability_locked = 0;
  if (job->host->capability_mutex != NULL &&
      h2_pal_mutex_lock(job->host->config.runtime->sync,
                        job->host->capability_mutex) == H2_PAL_OK) {
    capability_locked = 1;
  }
  for (i = 0u; i < job->host->config.max_coroutines_per_vm; ++i) {
    h2_lua_task_t *task = &job->tasks[i];
    if (task->state == H2_LUA_TASK_SLEEPING && now >= task->wake_ms) {
      h2_lua_task_timer_destroy(task);
      task->wake_ms = 0u;
      task->state = H2_LUA_TASK_READY;
    } else if (task->state == H2_LUA_TASK_SLEEPING &&
               atomic_load(&task->timer_fired) != 0) {
      h2_lua_task_timer_destroy(task);
      task->wake_ms = 0u;
      task->state = H2_LUA_TASK_READY;
    } else if (task->state == H2_LUA_TASK_JOINING) {
      h2_lua_task_t *target = h2_lua_find_task(job, task->join_task_id);
      if (target == NULL || task_is_terminal(target->state)) {
        task->state = H2_LUA_TASK_READY;
      }
    } else if (task->state == H2_LUA_TASK_CAPABILITY && capability_locked) {
      h2_lua_capability_request_t *request = h2_lua_find_capability_request(
          job->host, task->capability_request_id);
      if (request == NULL ||
          request->state == H2_LUA_CAPABILITY_REQUEST_COMPLETED ||
          request->state == H2_LUA_CAPABILITY_REQUEST_CANCELLED) {
        task->state = H2_LUA_TASK_READY;
      }
    }
  }
  if (capability_locked) {
    (void)h2_pal_mutex_unlock(job->host->config.runtime->sync,
                              job->host->capability_mutex);
  }
}

static h2_lua_task_t *next_ready_task(h2_lua_job_t *job) {
  size_t offset;
  size_t capacity = job->host->config.max_coroutines_per_vm;
  for (offset = 0u; offset < capacity; ++offset) {
    size_t index = (job->next_task_index + offset) % capacity;
    if (job->tasks[index].state == H2_LUA_TASK_READY) {
      job->next_task_index = (index + 1u) % capacity;
      return &job->tasks[index];
    }
  }
  return NULL;
}

static int has_ready_task(const h2_lua_job_t *job) {
  size_t i;
  for (i = 0u; i < job->host->config.max_coroutines_per_vm; ++i) {
    if (job->tasks[i].state == H2_LUA_TASK_READY) {
      return 1;
    }
  }
  return 0;
}

static void resume_task(h2_lua_job_t *job, h2_lua_task_t *task) {
  int result_count = 0;
  int status;
  int argument_count;
  const char *message;
  uint64_t now;
  if (job->cancel_requested || atomic_load(&job->host->stopping) != 0) {
    h2_lua_job_finish(job, H2_LUA_JOB_CANCELLED, "job cancelled");
    return;
  }
  now = h2_lua_now_ms(job->host);
  if (now - job->started_ms >= job->host->config.execution_timeout_ms) {
    h2_lua_job_finish(job, H2_LUA_JOB_TIMED_OUT, "job timed out");
    return;
  }
  if (task->cancel_requested) {
    h2_lua_task_timer_destroy(task);
    task->state = H2_LUA_TASK_CANCELLED;
    (void)snprintf(task->message, sizeof(task->message), "%s",
                   "task cancelled");
    if (task == &job->tasks[0]) {
      h2_lua_job_finish(job, H2_LUA_JOB_CANCELLED, task->message);
    }
    return;
  }
  job->state = H2_LUA_JOB_RUNNING;
  job->root_context.task = task;
  argument_count = task->resume_argument_count;
  task->resume_argument_count = 0;
  task->resume_started_ms = now;
  status = lua_resume(task->thread, NULL, argument_count, &result_count);
  job->root_context.task = NULL;
  job->resume_count++;
  if (status == LUA_YIELD) {
    if (task->state == H2_LUA_TASK_READY) {
      task->state = H2_LUA_TASK_READY;
    }
    if (result_count > 0) {
      lua_pop(task->thread, result_count);
    }
    return;
  }
  if (status == LUA_OK) {
    message = result_count > 0 ? lua_tostring(task->thread, -1) : NULL;
    task->state = H2_LUA_TASK_DONE;
    (void)snprintf(task->message, sizeof(task->message), "%s",
                   message == NULL ? "ok" : message);
    if (task == &job->tasks[0]) {
      h2_lua_job_finish(job, H2_LUA_JOB_SUCCEEDED, task->message);
    } else if (task->release_when_done) {
      luaL_unref(job->vm->state, LUA_REGISTRYINDEX, task->thread_ref);
      memset(task, 0, sizeof(*task));
      job->task_count--;
    }
    return;
  }
  message = lua_tostring(task->thread, -1);
  task->state = H2_LUA_TASK_FAILED;
  (void)snprintf(task->message, sizeof(task->message), "%s",
                 message == NULL ? "lua error" : message);
  if (task == &job->tasks[0]) {
    if (message != NULL && strstr(message, "timed out") != NULL) {
      h2_lua_job_finish(job, H2_LUA_JOB_TIMED_OUT, message);
    } else if (message != NULL && strstr(message, "cancelled") != NULL) {
      h2_lua_job_finish(job, H2_LUA_JOB_CANCELLED, message);
    } else {
      h2_lua_job_finish(job, H2_LUA_JOB_FAILED,
                        message == NULL ? "lua error" : message);
    }
  } else if (task->release_when_done) {
    luaL_unref(job->vm->state, LUA_REGISTRYINDEX, task->thread_ref);
    memset(task, 0, sizeof(*task));
    job->task_count--;
  }
}

h2_pal_result_t h2_lua_step_job(h2_lua_job_t *job) {
  uint64_t now;
  size_t i;
  if (job == NULL || job->host == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  now = h2_lua_now_ms(job->host);
  if (job->cancel_requested) {
    h2_lua_job_finish(job, H2_LUA_JOB_CANCELLED, "job cancelled");
  } else if (now - job->started_ms >= job->host->config.execution_timeout_ms) {
    h2_lua_job_finish(job, H2_LUA_JOB_TIMED_OUT, "job timed out");
  }
  if (is_terminal(job->state)) {
    for (i = 0u; i < job->host->config.max_coroutines_per_vm; ++i) {
      h2_lua_task_timer_destroy(&job->tasks[i]);
    }
    return H2_PAL_OK;
  }
  h2_lua_deliver_events(job);
  if (!is_terminal(job->state)) {
    update_waiters(job, now);
    h2_lua_task_t *task = next_ready_task(job);
    if (task != NULL) {
      resume_task(job, task);
      if (!is_terminal(job->state)) {
        job->state =
            has_ready_task(job) ? H2_LUA_JOB_QUEUED : H2_LUA_JOB_WAITING;
      }
    } else {
      job->state = H2_LUA_JOB_WAITING;
    }
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_lua_host_step(h2_lua_host_t *host) {
  uint8_t wake = 1u;
  if (host == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (atomic_load(&host->started) == 0) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  for (size_t i = 0u; i < host->config.worker_count; ++i) {
    (void)h2_pal_queue_send_latest(host->config.runtime->queue,
                                   host->workers[i].wake_queue, &wake);
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_lua_job_cancel(h2_lua_host_t *host, h2_lua_job_id_t job_id) {
  h2_lua_job_t *job = NULL;
  uint32_t job_generation;
  h2_pal_result_t result = h2_lua_lock_job(host, job_id, &job);
  if (result != H2_PAL_OK) {
    return result;
  }
  if (is_terminal(job->state)) {
    h2_lua_unlock_job(job);
    return H2_PAL_ERR_INVALID_STATE;
  }
  job->cancel_requested = 1;
  job_generation = job->generation;
  h2_lua_host_wake_job(job);
  h2_lua_unlock_job(job);
  h2_lua_cancel_job_capabilities(host, job_id, job_generation);
  return H2_PAL_OK;
}

h2_pal_result_t h2_lua_job_get_status(const h2_lua_host_t *host,
                                      h2_lua_job_id_t job_id,
                                      h2_lua_job_status_t *out_status) {
  h2_lua_job_t *job;
  if (host == NULL || out_status == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_result_t result = h2_lua_lock_job((h2_lua_host_t *)host, job_id, &job);
  if (result != H2_PAL_OK) {
    return result;
  }
  memset(out_status, 0, sizeof(*out_status));
  out_status->state = job->state;
  out_status->resume_count = job->resume_count;
  out_status->memory_used = h2_lua_vm_memory_used(job->vm);
  (void)snprintf(out_status->message, sizeof(out_status->message), "%s",
                 job->message);
  h2_lua_unlock_job(job);
  return H2_PAL_OK;
}

h2_pal_result_t h2_lua_job_release(h2_lua_host_t *host,
                                   h2_lua_job_id_t job_id) {
  h2_lua_job_t *job = NULL;
  const h2_pal_mem_api_t *mem;
  uint32_t job_generation;
  h2_pal_result_t result;
  if (host == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  result = h2_pal_mutex_lock(host->config.runtime->sync, host->jobs_mutex);
  if (result != H2_PAL_OK) {
    return result;
  }
  job = h2_lua_find_job(host, job_id);
  if (job == NULL) {
    (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs_mutex);
    return H2_PAL_ERR_NOT_FOUND;
  }
  result = h2_pal_mutex_lock(host->config.runtime->sync, job->mutex);
  if (result != H2_PAL_OK) {
    (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs_mutex);
    return result;
  }
  if (!is_terminal(job->state)) {
    h2_lua_unlock_job(job);
    (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs_mutex);
    return H2_PAL_ERR_BUSY;
  }
  mem = host->config.runtime->mem;
  job_generation = job->generation;
  for (size_t i = 0u; i < host->config.max_coroutines_per_vm; ++i) {
    h2_lua_task_timer_destroy(&job->tasks[i]);
  }
  if (job->display_open) {
    (void)h2_pal_display_close(host->config.runtime->display);
  }
  if (job->touch_open) {
    (void)h2_pal_touch_close(host->config.runtime->touch);
  }
  h2_lua_job_close_audio_tracks(job);
  h2_pal_mem_free(mem, job->framebuffer);
  h2_pal_mem_free(mem, job->tasks);
  h2_pal_mem_free(mem, job->callbacks);
  h2_pal_mem_free(mem, job->events);
  h2_pal_mem_free(mem, job->audio_tracks);
  h2_lua_vm_close(job->vm);
  h2_pal_mutex_t *job_mutex = job->mutex;
  memset(job, 0, sizeof(*job));
  job->mutex = job_mutex;
  (void)h2_pal_mutex_unlock(host->config.runtime->sync, job_mutex);
  (void)h2_pal_mutex_unlock(host->config.runtime->sync, host->jobs_mutex);
  h2_lua_release_job_capabilities(host, job_id, job_generation);
  return H2_PAL_OK;
}
