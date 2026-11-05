#include "h2_esp_board_sleep_enter.h"

#include <assert.h>
#include <stddef.h>

static int s_internal_stack, s_create_fails, s_leave_task_pending;
static int s_sleeps, s_creates, s_delays, s_deletes;
static uint32_t s_stack_depth;
static UBaseType_t s_priority;
static TaskFunction_t s_pending_task;
static void* s_pending_user;

bool esp_ptr_internal(const void* p) {
  assert(p != NULL);
  return s_internal_stack != 0;
}

bool esp_ptr_dma_capable(const void* p) {
  (void)p;
  return false;
}

void esp_deep_sleep_start(void) { ++s_sleeps; }

void vTaskDelay(TickType_t ticks) {
  assert(ticks == pdMS_TO_TICKS(H2_ZERO_ESP_SLEEP_ENTER_POLL_MS));
  ++s_delays;
}

void vTaskDelete(TaskHandle_t task) {
  assert(task == NULL); /* the handoff task deletes itself */
  ++s_deletes;
}

BaseType_t xTaskCreate(TaskFunction_t task, const char* name,
                       uint32_t stack_depth, void* parameters,
                       UBaseType_t priority, TaskHandle_t* created) {
  assert(name != NULL && created == NULL);
  if (s_create_fails) return 0;
  ++s_creates;
  s_stack_depth = stack_depth;
  s_priority = priority;
  /* FreeRTOS gives this task an internal-RAM stack; run it here unless the
   * case needs a task that is still inside esp_deep_sleep_start(). */
  if (s_leave_task_pending) {
    assert(s_pending_task == NULL);
    s_pending_task = task;
    s_pending_user = parameters;
  } else {
    task(parameters);
  }
  return pdPASS;
}

static void reset(void) {
  s_internal_stack = s_create_fails = s_leave_task_pending = 0;
  s_sleeps = s_creates = s_delays = s_deletes = 0;
  s_stack_depth = 0u;
  s_priority = 0u;
  s_pending_task = NULL;
  s_pending_user = NULL;
  h2_zero_esp_sleep_enter_state = H2_ZERO_ESP_SLEEP_ENTER_IDLE;
}

int main(void) {
  /* An internal stack keeps the direct call: the terminal commit path must
   * not allocate a task while the App is already dismantled. Returning from
   * the sleep call is itself the failure report. */
  reset();
  s_internal_stack = 1;
  assert(h2_zero_esp_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  assert(s_sleeps == 1 && s_creates == 0 && s_delays == 0 && s_deletes == 0);

  /* A PSRAM stack hands the final step to the internal-stack task. A refused
   * sleep is reported through the state, and the task frees its own stack
   * instead of running off the end of its function. */
  reset();
  assert(h2_zero_esp_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  assert(s_creates == 1 && s_sleeps == 1 && s_deletes == 1);
  assert(s_stack_depth == H2_ZERO_ESP_SLEEP_ENTER_TASK_STACK);
  assert(s_priority == (UBaseType_t)H2_ZERO_ESP_SLEEP_ENTER_TASK_PRIORITY);
  assert(s_delays == 0); /* completion is seen before the first wait */

  /* A second refused sleep must not inherit the previous completion. */
  assert(h2_zero_esp_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  assert(s_creates == 2 && s_sleeps == 2 && s_deletes == 2 && s_delays == 0);

  /* A task still inside esp_deep_sleep_start() reports nothing: the caller
   * times out after the bounded wait and leaves the task owning its stack. */
  reset();
  s_leave_task_pending = 1;
  assert(h2_zero_esp_sleep_enter_deep_sleep() == ESP_ERR_TIMEOUT);
  assert(s_creates == 1 && s_sleeps == 0 && s_deletes == 0);
  assert(s_delays == (int)(H2_ZERO_ESP_SLEEP_ENTER_TIMEOUT_MS /
                           H2_ZERO_ESP_SLEEP_ENTER_POLL_MS));
  /* A timed-out helper still owns the handoff. Another PSRAM caller may not
   * create a second helper, but terminal shutdown must retain the direct
   * internal-stack call, even if the earlier helper never reported back. */
  assert(h2_zero_esp_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  s_internal_stack = 1;
  assert(h2_zero_esp_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  s_internal_stack = 0;
  assert(s_creates == 1 && s_sleeps == 1 && s_deletes == 0);
  assert(h2_zero_esp_sleep_enter_state == H2_ZERO_ESP_SLEEP_ENTER_IN_FLIGHT);
  assert(h2_zero_esp_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  assert(s_creates == 1 && s_sleeps == 1);

  /* When the old helper finally returns, it reports failure and self-deletes.
   * A later retry may then claim a fresh handoff without stale completion. */
  assert(s_pending_task != NULL);
  TaskFunction_t pending_task = s_pending_task;
  void* pending_user = s_pending_user;
  s_pending_task = NULL;
  pending_task(pending_user);
  assert(s_sleeps == 2 && s_deletes == 1);
  s_leave_task_pending = 0;
  assert(h2_zero_esp_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  assert(s_creates == 2 && s_sleeps == 3 && s_deletes == 2);

  /* A failed handoff reports out of memory instead of waiting or sleeping. */
  reset();
  s_create_fails = 1;
  assert(h2_zero_esp_sleep_enter_deep_sleep() == ESP_ERR_NO_MEM);
  assert(s_sleeps == 0 && s_creates == 0 && s_delays == 0 && s_deletes == 0);
  return 0;
}
