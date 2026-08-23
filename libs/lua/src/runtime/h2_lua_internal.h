#ifndef H2_LUA_RUNTIME_INTERNAL_H
#define H2_LUA_RUNTIME_INTERNAL_H

#include "../core/h2_lua_internal.h"
#include "h2_lua_capability.h"
#include "h2_lua_event.h"
#include "h2_lua_job.h"
#include "h2_lua_module.h"

#include "lauxlib.h"

#include <stdatomic.h>

#define H2_LUA_NAME_MAX 48u
#define H2_LUA_MESSAGE_MAX 192u
#define H2_LUA_CAPABILITY_OUTPUT_MAX 512u
#define H2_LUA_PATH_MAX 192u

typedef enum h2_lua_task_state {
  H2_LUA_TASK_UNUSED = 0,
  H2_LUA_TASK_READY,
  H2_LUA_TASK_SLEEPING,
  H2_LUA_TASK_JOINING,
  H2_LUA_TASK_CAPABILITY,
  H2_LUA_TASK_DONE,
  H2_LUA_TASK_FAILED,
  H2_LUA_TASK_CANCELLED,
} h2_lua_task_state_t;

typedef struct h2_lua_execution_context {
  struct h2_lua_job *job;
  struct h2_lua_task *task;
} h2_lua_execution_context_t;

typedef struct h2_lua_task {
  h2_lua_execution_context_t context;
  struct h2_lua_job *job;
  uint32_t id;
  h2_lua_task_state_t state;
  lua_State *thread;
  int thread_ref;
  int resume_argument_count;
  uint64_t resume_started_ms;
  uint64_t wake_ms;
  h2_pal_timer_t *timer;
  atomic_int timer_fired;
  uint32_t join_task_id;
  h2_lua_capability_request_id_t capability_request_id;
  int cancel_requested;
  int release_when_done;
  char message[H2_LUA_MESSAGE_MAX];
} h2_lua_task_t;

typedef struct h2_lua_callback {
  uint32_t token;
  h2_runtime_component_id_t component_id;
  h2_runtime_event_kind_t kind;
  int lua_ref;
  int active;
} h2_lua_callback_t;

typedef struct h2_lua_event_record {
  h2_runtime_event_t event;
  uint8_t payload[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  size_t next_callback_index;
} h2_lua_event_record_t;

typedef struct h2_lua_module_entry {
  char name[H2_LUA_NAME_MAX];
  h2_lua_module_open_fn open_fn;
  void *user;
} h2_lua_module_entry_t;

typedef struct h2_lua_capability_entry {
  char name[H2_LUA_NAME_MAX];
  h2_lua_capability_call_fn call;
  h2_lua_capability_cancel_fn cancel;
  void *user;
} h2_lua_capability_entry_t;

typedef enum h2_lua_capability_request_state {
  H2_LUA_CAPABILITY_REQUEST_UNUSED = 0,
  H2_LUA_CAPABILITY_REQUEST_PENDING,
  H2_LUA_CAPABILITY_REQUEST_COMPLETED,
  H2_LUA_CAPABILITY_REQUEST_CANCELLED,
} h2_lua_capability_request_state_t;

typedef struct h2_lua_capability_request {
  h2_lua_capability_request_id_t id;
  h2_lua_capability_request_state_t state;
  h2_lua_job_id_t job_id;
  uint32_t job_generation;
  uint32_t task_id;
  h2_pal_result_t result;
  char output[H2_LUA_CAPABILITY_OUTPUT_MAX];
  char error[H2_LUA_MESSAGE_MAX];
  h2_lua_capability_entry_t *capability;
} h2_lua_capability_request_t;

typedef struct h2_lua_audio_track_slot {
  struct h2_lua_job *job;
  h2_pal_audio_track_t *track;
  h2_audio_pcm_format_t format;
  uint32_t generation;
  /* Carries the sub-frame tail of a write into the next one, so consecutive
   * writes form a gapless stream on Audio Systems with a fixed frame size.
   * Allocated lazily, holds fewer bytes than one device frame. */
  uint8_t *carry;
  size_t carry_bytes;
} h2_lua_audio_track_slot_t;

typedef struct h2_lua_job {
  h2_lua_execution_context_t root_context;
  struct h2_lua_host *host;
  h2_lua_job_id_t id;
  uint32_t generation;
  size_t worker_index;
  h2_pal_mutex_t *mutex;
  h2_lua_job_state_t state;
  h2_lua_vm_t *vm;
  h2_lua_task_t *tasks;
  size_t task_count;
  size_t next_task_index;
  uint32_t next_task_id;
  uint64_t started_ms;
  uint64_t resume_count;
  int cancel_requested;
  char message[H2_LUA_MESSAGE_MAX];
  h2_lua_callback_t *callbacks;
  size_t callback_count;
  uint32_t next_callback_token;
  h2_lua_event_record_t *events;
  size_t event_head;
  size_t event_count;
  uint16_t *framebuffer;
  h2_display_info_t display_info;
  int display_open;
  int frame_open;
  int dirty_valid;
  int dirty_min_x;
  int dirty_min_y;
  int dirty_max_x;
  int dirty_max_y;
  int touch_open;
  int touch_initialized;
  int touch_pressed;
  int touch_x;
  int touch_y;
  uint64_t touch_press_started_ms;
  h2_runtime_component_id_t button_component_id;
  h2_lua_audio_track_slot_t *audio_tracks;
  size_t active_audio_track_count;
  uint32_t next_audio_track_generation;
  int audio_speaker_acquired;
  char require_root[H2_LUA_PATH_MAX];
} h2_lua_job_t;

typedef struct h2_lua_worker {
  struct h2_lua_host *host;
  size_t index;
  h2_pal_task_t *task;
  h2_pal_queue_t *wake_queue;
} h2_lua_worker_t;

struct h2_lua_host {
  h2_lua_host_config_t config;
  h2_lua_job_t *jobs;
  h2_lua_job_id_t next_job_id;
  uint32_t next_job_generation;
  atomic_int started;
  atomic_int stopping;
  atomic_int joined;
  h2_lua_worker_t *workers;
  h2_pal_mutex_t *jobs_mutex;
  h2_lua_module_entry_t modules[16];
  size_t module_count;
  h2_lua_capability_entry_t capabilities[16];
  size_t capability_count;
  h2_lua_capability_request_t *capability_requests;
  h2_lua_capability_request_id_t next_capability_request_id;
  h2_pal_mutex_t *capability_mutex;
  h2_pal_mutex_t *audio_mutex;
  size_t audio_speaker_users;
};

void *h2_lua_runtime_realloc(void *user, void *ptr, size_t old_size,
                             size_t new_size);
h2_lua_job_t *h2_lua_find_job(h2_lua_host_t *host, h2_lua_job_id_t id);
h2_pal_result_t h2_lua_lock_job(h2_lua_host_t *host, h2_lua_job_id_t id,
                                h2_lua_job_t **out_job);
void h2_lua_unlock_job(h2_lua_job_t *job);
uint64_t h2_lua_now_ms(const h2_lua_host_t *host);
void h2_lua_job_finish(h2_lua_job_t *job, h2_lua_job_state_t state,
                       const char *message);
h2_pal_result_t h2_lua_register_builtin_modules(h2_lua_job_t *job);
void h2_lua_job_close_audio_tracks(h2_lua_job_t *job);
void h2_lua_audio_track_slot_flush_carry(h2_lua_audio_track_slot_t *slot);
void h2_lua_audio_track_slot_release_carry(h2_lua_audio_track_slot_t *slot,
                                           const h2_pal_mem_api_t *mem);
h2_pal_result_t h2_lua_job_acquire_audio_speaker(h2_lua_job_t *job);
void h2_lua_job_release_audio_speaker(h2_lua_job_t *job);
void h2_lua_deliver_events(h2_lua_job_t *job);
h2_lua_task_t *h2_lua_current_task(lua_State *state);
h2_lua_task_t *h2_lua_find_task(h2_lua_job_t *job, uint32_t task_id);
h2_pal_result_t h2_lua_spawn_task(h2_lua_job_t *job, lua_State *source_state,
                                  int function_index, int argument_count,
                                  uint32_t *out_task_id);
h2_lua_capability_request_t *
h2_lua_find_capability_request(h2_lua_host_t *host,
                               h2_lua_capability_request_id_t request_id);
void h2_lua_cancel_job_capabilities(h2_lua_host_t *host, h2_lua_job_id_t job_id,
                                    uint32_t job_generation);
void h2_lua_release_job_capabilities(h2_lua_host_t *host,
                                     h2_lua_job_id_t job_id,
                                     uint32_t job_generation);
void h2_lua_task_timer_destroy(h2_lua_task_t *task);
void h2_lua_host_wake_job(h2_lua_job_t *job);
h2_pal_result_t h2_lua_step_job(h2_lua_job_t *job);

#endif
