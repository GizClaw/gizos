#include "h2_app_test_memory.h"
#include "h2_lvgl_platform.h"
#include "h2_runtime_test.h"
#include "lvgl.h"

#include <stdlib.h>
#include <string.h>

struct h2_pal_mutex {
  unsigned int lock_depth;
  bool recursive;
};

typedef struct h2_app_test_memory {
  h2_app_test_app_t app;
  bool active;
  bool lvgl_started;
  h2_runtime_test_control_t *control;
} h2_app_test_memory_t;

static h2_app_test_memory_t *s_active_memory;

static size_t bounded_length(const char *value, size_t limit) {
  if (value == NULL) {
    return limit;
  }
  size_t length = 0u;
  while (length < limit && value[length] != '\0') {
    ++length;
  }
  return length;
}

_Static_assert(sizeof(h2_app_test_memory_t) <=
                   sizeof(((h2_app_test_driver_t *)0)->implementation_storage),
               "memory driver storage too small");

static void *memory_alloc(void *user, size_t len) {
  (void)user;
  return malloc(len);
}

static void *memory_realloc(void *user, void *ptr, size_t len) {
  (void)user;
  return realloc(ptr, len);
}

static void memory_free(void *user, void *ptr) {
  (void)user;
  free(ptr);
}

static h2_pal_result_t mutex_create(void *user,
                                    const h2_pal_mutex_config_t *config,
                                    h2_pal_mutex_t **mutex) {
  (void)user;
  if (config == NULL || mutex == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *mutex = calloc(1u, sizeof(**mutex));
  if (*mutex == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  (*mutex)->recursive = (config->flags & H2_PAL_MUTEX_FLAG_RECURSIVE) != 0u;
  return H2_PAL_OK;
}

static h2_pal_result_t mutex_destroy(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  if (mutex == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (mutex->lock_depth != 0u) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  free(mutex);
  return H2_PAL_OK;
}

static h2_pal_result_t mutex_lock(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  if (mutex == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (mutex->lock_depth != 0u && !mutex->recursive) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  ++mutex->lock_depth;
  return H2_PAL_OK;
}

static h2_pal_result_t mutex_try_lock(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  if (mutex == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (mutex->lock_depth != 0u && !mutex->recursive) {
    return H2_PAL_ERR_WOULD_BLOCK;
  }
  ++mutex->lock_depth;
  return H2_PAL_OK;
}

static h2_pal_result_t mutex_unlock(void *user, h2_pal_mutex_t *mutex) {
  (void)user;
  if (mutex == NULL || mutex->lock_depth == 0u) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  --mutex->lock_depth;
  return H2_PAL_OK;
}

static const h2_pal_mem_vtable_t s_mem_vtable = {
    .alloc = memory_alloc,
    .realloc = memory_realloc,
    .free = memory_free,
};

static const h2_pal_sync_vtable_t s_sync_vtable = {
    .create_mutex = mutex_create,
    .destroy_mutex = mutex_destroy,
    .lock_mutex = mutex_lock,
    .try_lock_mutex = mutex_try_lock,
    .unlock_mutex = mutex_unlock,
};

static const h2_pal_mem_api_t s_mem_api = {
    .user = NULL,
    .vtable = &s_mem_vtable,
};

static const h2_pal_task_api_t s_task_api = {0};
static const h2_pal_sync_api_t s_sync_api = {
    .user = NULL,
    .vtable = &s_sync_vtable,
};
static const h2_pal_queue_api_t s_queue_api = {0};
static const h2_pal_time_api_t s_time_api = {0};

static h2_pal_result_t headless_lvgl_start(h2_app_test_memory_t *memory) {
  if (s_active_memory != NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  const h2_lvgl_platform_config_t config = {
      .allocator = &s_mem_api,
      .task_api = &s_task_api,
      .sync_api = &s_sync_api,
      .queue_api = &s_queue_api,
      .time_api = &s_time_api,
  };
  h2_pal_result_t rc = h2_lvgl_platform_init(&config);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  lv_init();
  memory->lvgl_started = true;
  s_active_memory = memory;
  return H2_PAL_OK;
}

static void headless_lvgl_stop(h2_app_test_memory_t *memory) {
  if (!memory->lvgl_started) {
    return;
  }
  if (s_active_memory == memory) {
    lv_deinit();
    h2_lvgl_platform_deinit();
    s_active_memory = NULL;
  }
  memory->lvgl_started = false;
}

static h2_pal_result_t memory_open(void *user, const char *app_id,
                                   const h2_app_test_fixture_t *fixture) {
  h2_app_test_memory_t *memory = user;
  if (memory == NULL || memory->active || app_id == NULL ||
      memory->app.app_id == NULL || strcmp(memory->app.app_id, app_id) != 0) {
    return memory != NULL && memory->active ? H2_PAL_ERR_INVALID_STATE
                                            : H2_PAL_ERR_NOT_FOUND;
  }
  h2_pal_result_t rc = headless_lvgl_start(memory);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  rc = memory->app.vtable->reset(memory->app.user, fixture);
  if (rc == H2_PAL_OK) {
    h2_runtime_t *runtime = memory->app.vtable->runtime(memory->app.user);
    if (runtime == NULL) {
      rc = H2_PAL_ERR_INVALID_STATE;
    } else {
      rc = h2_runtime_test_control_open(runtime, &memory->control);
    }
  }
  if (rc == H2_PAL_OK) {
    memory->active = true;
    return H2_PAL_OK;
  }
  h2_runtime_test_control_close(memory->control);
  memory->control = NULL;
  memory->app.vtable->stop(memory->app.user);
  headless_lvgl_stop(memory);
  return rc;
}

static h2_pal_result_t memory_inject(
    h2_app_test_memory_t *memory,
    const h2_app_test_operation_t *operation) {
  switch (operation->kind) {
  case H2_APP_TEST_OPERATION_RUN:
    return H2_PAL_OK;
  case H2_APP_TEST_OPERATION_EVENT:
    return h2_runtime_test_emit_event(
        memory->control,
        operation->data.event.kind,
        operation->data.event.component,
        operation->data.event.component_id,
        operation->data.event.timestamp_ms,
        operation->data.event.payload,
        operation->data.event.payload_size);
  case H2_APP_TEST_OPERATION_COMPONENT_STATE:
    return h2_runtime_test_set_component_state(
        memory->control,
        operation->data.component_state.component_id,
        operation->data.component_state.data,
        operation->data.component_state.size);
  case H2_APP_TEST_OPERATION_BUTTON_DOWN:
    return h2_runtime_test_button_down(
        memory->control,
        operation->data.button.component_id,
        operation->data.button.pressed_at_ms);
  case H2_APP_TEST_OPERATION_BUTTON_UP:
    return h2_runtime_test_button_up(
        memory->control,
        operation->data.button.component_id,
        operation->data.button.pressed_at_ms,
        operation->data.button.released_at_ms);
  case H2_APP_TEST_OPERATION_BUTTON_ACTION:
    return h2_runtime_test_button_action(
        memory->control,
        operation->data.button.component_id,
        operation->data.button.pressed_at_ms,
        operation->data.button.released_at_ms,
        operation->data.button.click_count);
  default:
    return H2_PAL_ERR_INVALID_ARG;
  }
}

static h2_pal_result_t memory_execute(
    void *user, const h2_app_test_operation_t *operation,
    uint32_t generation, uint32_t timeout_ms,
    h2_app_test_snapshot_t *snapshot) {
  h2_app_test_memory_t *memory = user;
  if (memory == NULL || !memory->active || operation == NULL ||
      generation == 0u || snapshot == NULL) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  h2_pal_result_t rc = memory_inject(memory, operation);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  snapshot->generation = generation;
  snapshot->step_result =
      memory->app.vtable->run_step(memory->app.user, timeout_ms);
  h2_app_test_snapshot_writer_t writer = {.snapshot = snapshot};
  return memory->app.vtable->snapshot(memory->app.user, &writer);
}

static void memory_close(void *user) {
  h2_app_test_memory_t *memory = user;
  if (memory == NULL || !memory->active) {
    return;
  }
  h2_runtime_test_control_close(memory->control);
  memory->control = NULL;
  memory->app.vtable->stop(memory->app.user);
  memory->active = false;
  headless_lvgl_stop(memory);
}

static const h2_app_test_driver_vtable_t s_memory_vtable = {
    .open = memory_open,
    .execute = memory_execute,
    .close = memory_close,
};

h2_pal_result_t h2_app_test_memory_driver_init(h2_app_test_driver_t *driver,
                                               h2_app_test_app_t app) {
  if (driver == NULL || app.app_id == NULL || app.app_id[0] == '\0' ||
      bounded_length(app.app_id, H2_APP_TEST_APP_ID_MAX + 1u) >
          H2_APP_TEST_APP_ID_MAX ||
      app.vtable == NULL || app.vtable->reset == NULL ||
      app.vtable->runtime == NULL || app.vtable->run_step == NULL ||
      app.vtable->snapshot == NULL || app.vtable->stop == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(driver, 0, sizeof(*driver));
  h2_app_test_memory_t *memory =
      (h2_app_test_memory_t *)driver->implementation_storage;
  memory->app = app;
  driver->user = memory;
  driver->vtable = &s_memory_vtable;
  return H2_PAL_OK;
}

void h2_app_test_memory_driver_deinit(h2_app_test_driver_t *driver) {
  if (driver == NULL) {
    return;
  }
  h2_app_test_session_close(&driver->session);
  memset(driver, 0, sizeof(*driver));
}
