#include "h2_esp_tiga_call_monitor_controller.h"

#include <assert.h>

static void test_retries_worker_start(void) {
  h2_esp_tiga_call_monitor_controller_t controller;
  h2_esp_tiga_call_monitor_controller_init(&controller);

  assert(h2_esp_tiga_call_monitor_controller_observe(&controller, 1, true));
  assert(h2_esp_tiga_call_monitor_controller_next_action(&controller) ==
         H2_ESP_TIGA_CALL_MONITOR_ACTION_START);
  assert(h2_esp_tiga_call_monitor_controller_next_action(&controller) ==
         H2_ESP_TIGA_CALL_MONITOR_ACTION_START);

  h2_esp_tiga_call_monitor_controller_worker_started(&controller,
                                                     controller.generation);
  assert(h2_esp_tiga_call_monitor_controller_next_action(&controller) ==
         H2_ESP_TIGA_CALL_MONITOR_ACTION_NONE);
}

static void test_terminal_event_reaps_worker(void) {
  h2_esp_tiga_call_monitor_controller_t controller;
  h2_esp_tiga_call_monitor_controller_init(&controller);
  assert(h2_esp_tiga_call_monitor_controller_observe(&controller, 1, true));
  h2_esp_tiga_call_monitor_controller_worker_started(&controller,
                                                     controller.generation);

  assert(h2_esp_tiga_call_monitor_controller_observe(&controller, 1, false));
  assert(h2_esp_tiga_call_monitor_controller_next_action(&controller) ==
         H2_ESP_TIGA_CALL_MONITOR_ACTION_STOP);
  h2_esp_tiga_call_monitor_controller_worker_stop_requested(&controller);
  h2_esp_tiga_call_monitor_controller_worker_exited(&controller);
  assert(h2_esp_tiga_call_monitor_controller_next_action(&controller) ==
         H2_ESP_TIGA_CALL_MONITOR_ACTION_REAP);
  h2_esp_tiga_call_monitor_controller_worker_reaped(&controller);
  assert(h2_esp_tiga_call_monitor_controller_next_action(&controller) ==
         H2_ESP_TIGA_CALL_MONITOR_ACTION_NONE);
}

static void test_generation_replaces_old_worker(void) {
  h2_esp_tiga_call_monitor_controller_t controller;
  h2_esp_tiga_call_monitor_controller_init(&controller);
  assert(h2_esp_tiga_call_monitor_controller_observe(&controller, 1, true));
  const uint32_t first_generation = controller.generation;
  h2_esp_tiga_call_monitor_controller_worker_started(&controller,
                                                     first_generation);

  assert(h2_esp_tiga_call_monitor_controller_observe(&controller, 2, true));
  assert(controller.generation != first_generation);
  assert(!h2_esp_tiga_call_monitor_controller_commit_end(&controller,
                                                         first_generation));
  assert(h2_esp_tiga_call_monitor_controller_next_action(&controller) ==
         H2_ESP_TIGA_CALL_MONITOR_ACTION_STOP);
}

static void test_stop_reaps_in_flight_worker(void) {
  h2_esp_tiga_call_monitor_controller_t controller;
  h2_esp_tiga_call_monitor_controller_init(&controller);
  assert(h2_esp_tiga_call_monitor_controller_observe(&controller, 1, true));
  h2_esp_tiga_call_monitor_controller_worker_started(&controller,
                                                     controller.generation);

  h2_esp_tiga_call_monitor_controller_request_stop(&controller);
  assert(h2_esp_tiga_call_monitor_controller_next_action(&controller) ==
         H2_ESP_TIGA_CALL_MONITOR_ACTION_STOP);
  h2_esp_tiga_call_monitor_controller_worker_stop_requested(&controller);
  h2_esp_tiga_call_monitor_controller_worker_exited(&controller);
  assert(h2_esp_tiga_call_monitor_controller_next_action(&controller) ==
         H2_ESP_TIGA_CALL_MONITOR_ACTION_REAP);
  h2_esp_tiga_call_monitor_controller_worker_reaped(&controller);
  assert(h2_esp_tiga_call_monitor_controller_next_action(&controller) ==
         H2_ESP_TIGA_CALL_MONITOR_ACTION_EXIT);
}

int main(void) {
  test_retries_worker_start();
  test_terminal_event_reaps_worker();
  test_generation_replaces_old_worker();
  test_stop_reaps_in_flight_worker();
  return 0;
}
