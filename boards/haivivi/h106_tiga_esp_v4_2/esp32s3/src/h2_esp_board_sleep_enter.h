#ifndef H2_ESP_BOARD_SLEEP_ENTER_H
#define H2_ESP_BOARD_SLEEP_ENTER_H

#include "esp_err.h"
#include "esp_memory_utils.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Both terminal paths hold digital IOs, so esp_deep_sleep_start() always runs
 * esp_sleep_isolate_digital_gpio(). That function isolates the MSPI pads, so
 * IDF 6 asserts that the stack of the calling task lives in internal RAM: a
 * PSRAM stack can no longer be read once the pads are gone. The supervisor
 * commit already runs on the internal-stack "h106/shutdown" task, but the
 * plain PAL deep-sleep request runs on the "h106/modem" worker, whose stack is
 * in PSRAM. Hand the final step to a short-lived task instead of moving whole
 * workers into internal RAM: FreeRTOS allocates task stacks with
 * MALLOC_CAP_INTERNAL, so this one always satisfies the assert. */
#define H2_TIGA_SLEEP_ENTER_TASK_NAME "h2/deep-sleep"
#define H2_TIGA_SLEEP_ENTER_TASK_STACK 4096u
#define H2_TIGA_SLEEP_ENTER_TASK_PRIORITY (configMAX_PRIORITIES - 1)
/* Only bounds a sleep that never starts; normally the chip stops inside. */
#define H2_TIGA_SLEEP_ENTER_TIMEOUT_MS 3000u
#define H2_TIGA_SLEEP_ENTER_POLL_MS 10u

/* The state is board-owned and protected across the two ESP cores. A timed-out
 * caller leaves IN_FLIGHT intact: only the helper may release its handoff
 * after esp_deep_sleep_start() has returned. */
enum {
  H2_TIGA_SLEEP_ENTER_IDLE,
  H2_TIGA_SLEEP_ENTER_IN_FLIGHT,
  H2_TIGA_SLEEP_ENTER_RETURNED,
};
static portMUX_TYPE h2_tiga_sleep_enter_lock = portMUX_INITIALIZER_UNLOCKED;
static int h2_tiga_sleep_enter_state = H2_TIGA_SLEEP_ENTER_IDLE;

static int h2_tiga_sleep_enter_get_state(void) {
  portENTER_CRITICAL(&h2_tiga_sleep_enter_lock);
  int state = h2_tiga_sleep_enter_state;
  portEXIT_CRITICAL(&h2_tiga_sleep_enter_lock);
  return state;
}

static bool h2_tiga_sleep_enter_claim(void) {
  portENTER_CRITICAL(&h2_tiga_sleep_enter_lock);
  bool claimed = h2_tiga_sleep_enter_state != H2_TIGA_SLEEP_ENTER_IN_FLIGHT;
  if (claimed) h2_tiga_sleep_enter_state = H2_TIGA_SLEEP_ENTER_IN_FLIGHT;
  portEXIT_CRITICAL(&h2_tiga_sleep_enter_lock);
  return claimed;
}

static void h2_tiga_sleep_enter_finish(void) {
  portENTER_CRITICAL(&h2_tiga_sleep_enter_lock);
  h2_tiga_sleep_enter_state = H2_TIGA_SLEEP_ENTER_RETURNED;
  portEXIT_CRITICAL(&h2_tiga_sleep_enter_lock);
}

static void h2_tiga_sleep_enter_task(void* user) {
  (void)user;
  esp_deep_sleep_start();
  h2_tiga_sleep_enter_finish();
  /* The task owns its own stack: self-deleting returns that internal RAM, so
   * a refused sleep cannot leak a stack per attempt. */
  vTaskDelete(NULL);
}

/* Enters deep sleep; it does not return when the chip stops. Returns
 * ESP_ERR_INVALID_STATE when esp_deep_sleep_start() itself returned,
 * ESP_ERR_NO_MEM when the internal-stack task could not be created, and
 * ESP_ERR_TIMEOUT when the handoff task neither slept nor reported back.
 * A PSRAM-stack retry while that helper is still active returns
 * ESP_ERR_INVALID_STATE without starting another helper. Callers whose stack
 * is already internal keep the previous direct call even during a stuck
 * handoff: the terminal supervisor must always be able to enter sleep. */
static inline esp_err_t h2_tiga_sleep_enter_deep_sleep(void) {
  volatile char stack_probe = 0;
  if (esp_ptr_internal((const void*)&stack_probe)) {
    esp_deep_sleep_start();
    return ESP_ERR_INVALID_STATE;
  }
  if (!h2_tiga_sleep_enter_claim()) return ESP_ERR_INVALID_STATE;
  if (xTaskCreate(h2_tiga_sleep_enter_task, H2_TIGA_SLEEP_ENTER_TASK_NAME,
                  H2_TIGA_SLEEP_ENTER_TASK_STACK, NULL,
                  H2_TIGA_SLEEP_ENTER_TASK_PRIORITY, NULL) != pdPASS) {
    h2_tiga_sleep_enter_finish();
    return ESP_ERR_NO_MEM;
  }
  for (uint32_t waited = 0u; waited < H2_TIGA_SLEEP_ENTER_TIMEOUT_MS;
       waited += H2_TIGA_SLEEP_ENTER_POLL_MS) {
    if (h2_tiga_sleep_enter_get_state() == H2_TIGA_SLEEP_ENTER_RETURNED)
      return ESP_ERR_INVALID_STATE;
    vTaskDelay(pdMS_TO_TICKS(H2_TIGA_SLEEP_ENTER_POLL_MS));
  }
  if (h2_tiga_sleep_enter_get_state() == H2_TIGA_SLEEP_ENTER_RETURNED)
    return ESP_ERR_INVALID_STATE;
  /* The task is still inside esp_deep_sleep_start() and owns its stack, so it
   * must not be deleted from here; it self-deletes if that call ever returns.
   */
  return ESP_ERR_TIMEOUT;
}

#endif
