#include "h2_esp_board_sleep_enter.h"

#include <assert.h>
#include <stddef.h>

static int s_internal_stack, s_create_fails, s_leave_task_pending;
static unsigned s_sdk_delay;
static int s_defer_sdk;
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

static void dispatch(void) {
  if (s_pending_task == NULL) return;
  TaskFunction_t task = s_pending_task;
  void* user = s_pending_user;
  s_pending_task = NULL;
  task(user);
}

void vTaskDelay(TickType_t ticks) {
  assert(ticks == pdMS_TO_TICKS(H2_TIGA_SLEEP_ENTER_POLL_MS));
  ++s_delays;
  /* Drive the actual admission/finish transitions around a delayed SDK call.
   * Its completion occurs after the queued-start deadline has elapsed. */
  if (s_sdk_delay) {
    if (--s_sdk_delay == 0) {
      h2_tiga_sleep_enter_finish();
      vTaskDelete(NULL);
    }
    return;
  }
  if (s_defer_sdk && s_pending_task) {
    assert(h2_tiga_sleep_enter_begin());
    s_pending_task = NULL;
    ++s_sleeps;
    s_sdk_delay =
        H2_TIGA_SLEEP_ENTER_TIMEOUT_MS / H2_TIGA_SLEEP_ENTER_POLL_MS + 5u;
    return;
  }
  if (!s_leave_task_pending &&
      h2_tiga_sleep_enter_get_state() != H2_TIGA_SLEEP_ENTER_RESERVED)
    dispatch();
}
void vTaskDelete(TaskHandle_t task) {
  assert(task == NULL);
  ++s_deletes;
}
BaseType_t xTaskCreate(TaskFunction_t task, const char* name,
                       uint32_t stack_depth, void* parameters,
                       UBaseType_t priority, TaskHandle_t* created) {
  assert(name != NULL && created == NULL && s_pending_task == NULL);
  if (s_create_fails) return 0;
  ++s_creates;
  s_stack_depth = stack_depth;
  s_priority = priority;
  s_pending_task = task;
  s_pending_user = parameters;
  return pdPASS;
}
static void reset(void) {
  assert(s_pending_task == NULL);
  s_internal_stack = s_create_fails = s_leave_task_pending = 0;
  s_sdk_delay = 0;
  s_defer_sdk = 0;
  s_sleeps = s_creates = s_delays = s_deletes = 0;
  s_stack_depth = 0;
  s_priority = 0;
  h2_tiga_sleep_enter_state = H2_TIGA_SLEEP_ENTER_IDLE;
}

int main(void) {
  reset();
  s_internal_stack = 1;
  assert(h2_tiga_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  assert(s_sleeps == 1 && s_creates == 0 && s_deletes == 0);

  /* The fallible reservation precedes all hardware preparation and cannot
   * invoke sleep. Allocation failure needs no GPIO/power rollback. */
  reset();
  s_create_fails = 1;
  assert(h2_tiga_sleep_prepare_deep_sleep() == ESP_ERR_NO_MEM);
  assert(s_sleeps == 0 && s_creates == 0 && s_delays == 0);
  s_create_fails = 0;
  assert(h2_tiga_sleep_prepare_deep_sleep() == ESP_OK);
  assert(s_creates == 1 && s_sleeps == 0 && s_deletes == 0);
  assert(h2_tiga_sleep_prepare_deep_sleep() == ESP_ERR_INVALID_STATE);
  assert(s_stack_depth == H2_TIGA_SLEEP_ENTER_TASK_STACK);
  assert(s_priority == (UBaseType_t)H2_TIGA_SLEEP_ENTER_TASK_PRIORITY);

  /* Hardware preparation can fail after reservation: cancellation must never
   * let the delayed helper sleep after the caller has resumed its App. */
  h2_tiga_sleep_cancel_prepared();
  assert(h2_tiga_sleep_prepare_deep_sleep() == ESP_ERR_INVALID_STATE);
  dispatch();
  assert(s_sleeps == 0 && s_deletes == 1);
  assert(h2_tiga_sleep_prepare_deep_sleep() == ESP_OK);
  assert(h2_tiga_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  assert(s_creates == 2 && s_sleeps == 1 && s_deletes == 2);

  reset();
  assert(h2_tiga_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  assert(s_creates == 1 && s_sleeps == 1 && s_deletes == 1);
  assert(h2_tiga_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  assert(s_creates == 2 && s_sleeps == 2 && s_deletes == 2);

  reset();
  s_leave_task_pending = 1;
  assert(h2_tiga_sleep_enter_deep_sleep() == ESP_ERR_TIMEOUT);
  assert(s_creates == 1 && s_sleeps == 0 && s_deletes == 0);
  assert(h2_tiga_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  /* A terminal internal-stack supervisor retains its allocation-free path. */
  s_internal_stack = 1;
  assert(h2_tiga_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  s_internal_stack = 0;
  assert(s_creates == 1 && s_sleeps == 1 && s_deletes == 0);
  assert(h2_tiga_sleep_enter_state == H2_TIGA_SLEEP_ENTER_CANCELLED);
  dispatch();
  assert(s_sleeps == 1 &&
         s_deletes == 1); /* delayed cancelled helper cannot sleep */
  s_leave_task_pending = 0;
  assert(h2_tiga_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  assert(s_creates == 2 && s_sleeps == 2 && s_deletes == 2);
  reset();
  s_defer_sdk = 1;
  assert(h2_tiga_sleep_enter_deep_sleep() == ESP_ERR_INVALID_STATE);
  assert(s_delays >
         (int)(H2_TIGA_SLEEP_ENTER_TIMEOUT_MS / H2_TIGA_SLEEP_ENTER_POLL_MS));
  assert(s_sleeps == 1 && s_deletes == 1 && s_pending_task == NULL);
  return 0;
}
