#ifndef H2_ESP_TIGA_CALL_MONITOR_CONTROLLER_H
#define H2_ESP_TIGA_CALL_MONITOR_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum h2_esp_tiga_call_monitor_action {
  H2_ESP_TIGA_CALL_MONITOR_ACTION_NONE = 0,
  H2_ESP_TIGA_CALL_MONITOR_ACTION_START,
  H2_ESP_TIGA_CALL_MONITOR_ACTION_STOP,
  H2_ESP_TIGA_CALL_MONITOR_ACTION_REAP,
  H2_ESP_TIGA_CALL_MONITOR_ACTION_EXIT,
} h2_esp_tiga_call_monitor_action_t;

typedef enum h2_esp_tiga_call_monitor_worker_state {
  H2_ESP_TIGA_CALL_MONITOR_WORKER_NONE = 0,
  H2_ESP_TIGA_CALL_MONITOR_WORKER_RUNNING,
  H2_ESP_TIGA_CALL_MONITOR_WORKER_STOP_REQUESTED,
  H2_ESP_TIGA_CALL_MONITOR_WORKER_EXITED,
} h2_esp_tiga_call_monitor_worker_state_t;

typedef struct h2_esp_tiga_call_monitor_controller {
  bool desired_active;
  bool stopping;
  int32_t call_id;
  uint32_t generation;
  uint32_t worker_generation;
  h2_esp_tiga_call_monitor_worker_state_t worker_state;
} h2_esp_tiga_call_monitor_controller_t;

void h2_esp_tiga_call_monitor_controller_init(
    h2_esp_tiga_call_monitor_controller_t* controller);
bool h2_esp_tiga_call_monitor_controller_observe(
    h2_esp_tiga_call_monitor_controller_t* controller, int32_t call_id,
    bool active);
bool h2_esp_tiga_call_monitor_controller_commit_end(
    h2_esp_tiga_call_monitor_controller_t* controller,
    uint32_t worker_generation);
void h2_esp_tiga_call_monitor_controller_request_stop(
    h2_esp_tiga_call_monitor_controller_t* controller);
void h2_esp_tiga_call_monitor_controller_worker_started(
    h2_esp_tiga_call_monitor_controller_t* controller, uint32_t generation);
void h2_esp_tiga_call_monitor_controller_worker_stop_requested(
    h2_esp_tiga_call_monitor_controller_t* controller);
void h2_esp_tiga_call_monitor_controller_worker_exited(
    h2_esp_tiga_call_monitor_controller_t* controller);
void h2_esp_tiga_call_monitor_controller_worker_reaped(
    h2_esp_tiga_call_monitor_controller_t* controller);
h2_esp_tiga_call_monitor_action_t
h2_esp_tiga_call_monitor_controller_next_action(
    const h2_esp_tiga_call_monitor_controller_t* controller);

#endif
