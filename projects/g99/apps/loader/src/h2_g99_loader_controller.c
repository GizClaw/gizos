#include "h2_g99_loader_controller.h"

#include "h2_g99_loader_ui.h"

#include <string.h>

typedef enum controller_event_kind {
  CONTROLLER_INSTALLING,
  CONTROLLER_PROGRESS,
  CONTROLLER_FAILED,
  CONTROLLER_LAUNCHING,
  CONTROLLER_OBSERVE,
} controller_event_kind_t;

typedef struct controller_event {
  controller_event_kind_t kind;
  h2_loader_install_phase_t phase;
  uint64_t completed;
  uint64_t total;
  int code;
} controller_event_t;

typedef struct controller_response {
  h2_g99_loader_state_t state;
  h2_pal_result_t result;
} controller_response_t;

struct h2_g99_loader_controller {
  h2_runtime_t* runtime;
  h2_pal_queue_t* events;
  h2_pal_mutex_t* submit_mutex;
  h2_pal_queue_t* responses;
  h2_pal_task_t* task;
};

static void render_task(void* user) {
  h2_g99_loader_controller_t* controller = user;
  h2_runtime_t* runtime = controller->runtime;
  h2_g99_loader_ui_t* ui = NULL;
  controller_response_t response = {0};
  response.result = h2_g99_loader_ui_open(runtime, &ui);
  const h2_pal_result_t opened = response.result;
  if (ui != NULL) h2_g99_loader_ui_state(ui, &response.state);
  const h2_pal_result_t acknowledged =
      h2_pal_queue_send(runtime->queue, controller->responses, &response,
                        H2_PAL_QUEUE_WAIT_FOREVER);
  if (opened == H2_PAL_OK && acknowledged == H2_PAL_OK) {
    controller_event_t event;
    while (h2_pal_queue_recv(runtime->queue, controller->events, &event,
                             H2_PAL_QUEUE_WAIT_FOREVER) == H2_PAL_OK) {
      switch (event.kind) {
        case CONTROLLER_INSTALLING:
          response.result = h2_g99_loader_ui_installing(ui);
          break;
        case CONTROLLER_PROGRESS:
          response.result = h2_g99_loader_ui_progress(
              ui, event.phase, event.completed, event.total);
          break;
        case CONTROLLER_FAILED:
          response.result = h2_g99_loader_ui_failed(ui, event.code);
          break;
        case CONTROLLER_LAUNCHING:
          response.result = h2_g99_loader_ui_launching(ui);
          break;
        case CONTROLLER_OBSERVE:
          response.result = H2_PAL_OK;
          break;
      }
      h2_g99_loader_ui_state(ui, &response.state);
      if (h2_pal_queue_send(runtime->queue, controller->responses, &response,
                            H2_PAL_QUEUE_WAIT_FOREVER) != H2_PAL_OK)
        break;
    }
  }
  (void)h2_pal_queue_close(runtime->queue, controller->events);
  (void)h2_pal_queue_close(runtime->queue, controller->responses);
  h2_g99_loader_ui_close(ui);
}

static h2_pal_result_t submit(h2_g99_loader_controller_t* controller,
                              const controller_event_t* event,
                              h2_g99_loader_state_t* out_state) {
  if (controller == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_runtime_t* runtime = controller->runtime;
  h2_pal_result_t rc =
      h2_pal_mutex_lock(runtime->sync, controller->submit_mutex);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_pal_queue_send(runtime->queue, controller->events, event,
                         H2_PAL_QUEUE_WAIT_FOREVER);
  controller_response_t response;
  if (rc == H2_PAL_OK)
    rc = h2_pal_queue_recv(runtime->queue, controller->responses, &response,
                           H2_PAL_QUEUE_WAIT_FOREVER);
  if (rc == H2_PAL_OK) {
    rc = response.result;
    if (out_state != NULL) *out_state = response.state;
  }
  const h2_pal_result_t unlocked =
      h2_pal_mutex_unlock(runtime->sync, controller->submit_mutex);
  return rc != H2_PAL_OK ? rc : unlocked;
}

h2_pal_result_t h2_g99_loader_controller_open(
    h2_runtime_t* runtime, h2_g99_loader_controller_t** out_controller) {
  if (out_controller == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_controller = NULL;
  if (runtime == NULL || runtime->mem == NULL || runtime->sync == NULL ||
      runtime->task == NULL || runtime->queue == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_g99_loader_controller_t* controller =
      h2_pal_mem_alloc(runtime->mem, sizeof(*controller));
  if (controller == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(controller, 0, sizeof(*controller));
  controller->runtime = runtime;
  *out_controller = controller;
  const h2_pal_mutex_config_t mutex = {.name = "g99-loader-submit",
                                       .allocator = runtime->mem};
  const h2_pal_queue_config_t responses = {
      .name = "g99-loader-rendered",
      .item_size = sizeof(controller_response_t),
      .item_count = 1u,
      .allocator = runtime->mem};
  const h2_pal_queue_config_t queue = {.name = "g99-loader-events",
                                       .item_size = sizeof(controller_event_t),
                                       .item_count = 1u,
                                       .allocator = runtime->mem};
  const h2_pal_task_options_t task = {.name = "g99/loader/ui",
                                      .min_stack_size = 65536u};
  h2_pal_result_t rc =
      h2_pal_mutex_create(runtime->sync, &mutex, &controller->submit_mutex);
  if (rc == H2_PAL_OK)
    rc =
        h2_pal_queue_create(runtime->queue, &responses, &controller->responses);
  if (rc == H2_PAL_OK)
    rc = h2_pal_queue_create(runtime->queue, &queue, &controller->events);
  if (rc == H2_PAL_OK)
    rc = h2_pal_task_start(runtime->task, &task, render_task, controller,
                           &controller->task);
  controller_response_t response;
  if (rc == H2_PAL_OK)
    rc = h2_pal_queue_recv(runtime->queue, controller->responses, &response,
                           H2_PAL_QUEUE_WAIT_FOREVER);
  if (rc == H2_PAL_OK) rc = response.result;
  if (rc != H2_PAL_OK) (void)h2_g99_loader_controller_close(out_controller);
  return rc;
}

h2_pal_result_t h2_g99_loader_controller_installing(
    h2_g99_loader_controller_t* controller) {
  const controller_event_t event = {.kind = CONTROLLER_INSTALLING};
  return submit(controller, &event, NULL);
}

h2_pal_result_t h2_g99_loader_controller_progress(
    h2_g99_loader_controller_t* controller, h2_loader_install_phase_t phase,
    uint64_t completed, uint64_t total) {
  const controller_event_t event = {.kind = CONTROLLER_PROGRESS,
                                    .phase = phase,
                                    .completed = completed,
                                    .total = total};
  return submit(controller, &event, NULL);
}

h2_pal_result_t h2_g99_loader_controller_failed(
    h2_g99_loader_controller_t* controller, int code) {
  const controller_event_t event = {.kind = CONTROLLER_FAILED, .code = code};
  return submit(controller, &event, NULL);
}

h2_pal_result_t h2_g99_loader_controller_launching(
    h2_g99_loader_controller_t* controller) {
  const controller_event_t event = {.kind = CONTROLLER_LAUNCHING};
  return submit(controller, &event, NULL);
}

h2_pal_result_t h2_g99_loader_controller_state(
    h2_g99_loader_controller_t* controller, h2_g99_loader_state_t* out_state) {
  if (out_state == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_state, 0, sizeof(*out_state));
  const controller_event_t event = {.kind = CONTROLLER_OBSERVE};
  return submit(controller, &event, out_state);
}

h2_pal_result_t h2_g99_loader_controller_close(
    h2_g99_loader_controller_t** owner) {
  if (owner == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_g99_loader_controller_t* controller = *owner;
  if (controller == NULL) return H2_PAL_OK;
  h2_runtime_t* runtime = controller->runtime;
  h2_pal_result_t rc;
  if (controller->events != NULL) {
    rc = h2_pal_queue_close(runtime->queue, controller->events);
    if (rc != H2_PAL_OK) return rc;
  }
  if (controller->task != NULL) {
    rc = h2_pal_task_join(runtime->task, controller->task);
    if (rc != H2_PAL_OK) return rc;
    controller->task = NULL;
  }
  if (controller->events != NULL) {
    h2_pal_queue_destroy(runtime->queue, controller->events);
    controller->events = NULL;
  }
  if (controller->responses != NULL) {
    h2_pal_queue_destroy(runtime->queue, controller->responses);
    controller->responses = NULL;
  }
  if (controller->submit_mutex != NULL) {
    rc = h2_pal_mutex_destroy(runtime->sync, controller->submit_mutex);
    if (rc != H2_PAL_OK) return rc;
    controller->submit_mutex = NULL;
  }
  h2_pal_mem_free(runtime->mem, controller);
  *owner = NULL;
  return H2_PAL_OK;
}
