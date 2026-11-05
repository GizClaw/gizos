#include "h2_esp_tiga_call_monitor_controller.h"

#include <string.h>

void h2_esp_tiga_call_monitor_controller_init(
    h2_esp_tiga_call_monitor_controller_t* controller) {
  if (controller != NULL) {
    memset(controller, 0, sizeof(*controller));
  }
}

bool h2_esp_tiga_call_monitor_controller_observe(
    h2_esp_tiga_call_monitor_controller_t* controller, int32_t call_id,
    bool active) {
  if (controller == NULL) {
    return false;
  }
  if (active) {
    if (!controller->desired_active || controller->call_id != call_id) {
      controller->generation += 1u;
    }
    controller->call_id = call_id;
    controller->desired_active = true;
    return true;
  }
  if (!controller->desired_active || controller->call_id != call_id) {
    return false;
  }
  controller->desired_active = false;
  return true;
}

bool h2_esp_tiga_call_monitor_controller_commit_end(
    h2_esp_tiga_call_monitor_controller_t* controller,
    uint32_t worker_generation) {
  if (controller == NULL || !controller->desired_active ||
      controller->generation != worker_generation) {
    return false;
  }
  controller->desired_active = false;
  return true;
}

void h2_esp_tiga_call_monitor_controller_request_stop(
    h2_esp_tiga_call_monitor_controller_t* controller) {
  if (controller != NULL) {
    controller->stopping = true;
    controller->desired_active = false;
  }
}

void h2_esp_tiga_call_monitor_controller_worker_started(
    h2_esp_tiga_call_monitor_controller_t* controller, uint32_t generation) {
  if (controller != NULL) {
    controller->worker_generation = generation;
    controller->worker_state = H2_ESP_TIGA_CALL_MONITOR_WORKER_RUNNING;
  }
}

void h2_esp_tiga_call_monitor_controller_worker_stop_requested(
    h2_esp_tiga_call_monitor_controller_t* controller) {
  if (controller != NULL &&
      controller->worker_state == H2_ESP_TIGA_CALL_MONITOR_WORKER_RUNNING) {
    controller->worker_state = H2_ESP_TIGA_CALL_MONITOR_WORKER_STOP_REQUESTED;
  }
}

void h2_esp_tiga_call_monitor_controller_worker_exited(
    h2_esp_tiga_call_monitor_controller_t* controller) {
  if (controller != NULL &&
      controller->worker_state != H2_ESP_TIGA_CALL_MONITOR_WORKER_NONE) {
    controller->worker_state = H2_ESP_TIGA_CALL_MONITOR_WORKER_EXITED;
  }
}

void h2_esp_tiga_call_monitor_controller_worker_reaped(
    h2_esp_tiga_call_monitor_controller_t* controller) {
  if (controller != NULL) {
    controller->worker_generation = 0u;
    controller->worker_state = H2_ESP_TIGA_CALL_MONITOR_WORKER_NONE;
  }
}

h2_esp_tiga_call_monitor_action_t
h2_esp_tiga_call_monitor_controller_next_action(
    const h2_esp_tiga_call_monitor_controller_t* controller) {
  if (controller == NULL) {
    return H2_ESP_TIGA_CALL_MONITOR_ACTION_NONE;
  }
  if (controller->worker_state == H2_ESP_TIGA_CALL_MONITOR_WORKER_EXITED) {
    return H2_ESP_TIGA_CALL_MONITOR_ACTION_REAP;
  }
  if (controller->worker_state == H2_ESP_TIGA_CALL_MONITOR_WORKER_RUNNING &&
      (!controller->desired_active || controller->stopping ||
       controller->worker_generation != controller->generation)) {
    return H2_ESP_TIGA_CALL_MONITOR_ACTION_STOP;
  }
  if (controller->worker_state == H2_ESP_TIGA_CALL_MONITOR_WORKER_NONE &&
      controller->desired_active && !controller->stopping) {
    return H2_ESP_TIGA_CALL_MONITOR_ACTION_START;
  }
  if (controller->stopping &&
      controller->worker_state == H2_ESP_TIGA_CALL_MONITOR_WORKER_NONE) {
    return H2_ESP_TIGA_CALL_MONITOR_ACTION_EXIT;
  }
  return H2_ESP_TIGA_CALL_MONITOR_ACTION_NONE;
}
