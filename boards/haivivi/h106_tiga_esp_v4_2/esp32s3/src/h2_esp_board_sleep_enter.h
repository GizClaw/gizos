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

/* GPIO isolation makes PSRAM inaccessible. Reserve an internal-stack helper
 * before any peripheral shutdown or pad holds; it cannot sleep until commit. */
#define H2_TIGA_SLEEP_ENTER_TASK_NAME "h2/deep-sleep"
#define H2_TIGA_SLEEP_ENTER_TASK_STACK 4096u
#define H2_TIGA_SLEEP_ENTER_TASK_PRIORITY (configMAX_PRIORITIES - 1)
#define H2_TIGA_SLEEP_ENTER_TIMEOUT_MS 3000u
#define H2_TIGA_SLEEP_ENTER_POLL_MS 10u

enum {
  H2_TIGA_SLEEP_ENTER_IDLE,
  H2_TIGA_SLEEP_ENTER_RESERVED,
  H2_TIGA_SLEEP_ENTER_QUEUED,
  H2_TIGA_SLEEP_ENTER_IN_FLIGHT,
  H2_TIGA_SLEEP_ENTER_CANCELLED,
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
  bool claimed = h2_tiga_sleep_enter_state == H2_TIGA_SLEEP_ENTER_IDLE ||
                 h2_tiga_sleep_enter_state == H2_TIGA_SLEEP_ENTER_RETURNED;
  if (claimed) h2_tiga_sleep_enter_state = H2_TIGA_SLEEP_ENTER_RESERVED;
  portEXIT_CRITICAL(&h2_tiga_sleep_enter_lock);
  return claimed;
}

static void h2_tiga_sleep_enter_finish(void) {
  portENTER_CRITICAL(&h2_tiga_sleep_enter_lock);
  h2_tiga_sleep_enter_state = H2_TIGA_SLEEP_ENTER_RETURNED;
  portEXIT_CRITICAL(&h2_tiga_sleep_enter_lock);
}

static bool h2_tiga_sleep_enter_begin(void) {
  portENTER_CRITICAL(&h2_tiga_sleep_enter_lock);
  bool start = h2_tiga_sleep_enter_state == H2_TIGA_SLEEP_ENTER_QUEUED;
  if (start) h2_tiga_sleep_enter_state = H2_TIGA_SLEEP_ENTER_IN_FLIGHT;
  portEXIT_CRITICAL(&h2_tiga_sleep_enter_lock);
  return start;
}

static void h2_tiga_sleep_enter_task(void* user) {
  (void)user;
  while (h2_tiga_sleep_enter_get_state() == H2_TIGA_SLEEP_ENTER_RESERVED)
    vTaskDelay(pdMS_TO_TICKS(H2_TIGA_SLEEP_ENTER_POLL_MS));
  if (h2_tiga_sleep_enter_begin()) esp_deep_sleep_start();
  /* Cancelled helpers never touch hardware. No caller borrows their stack. */
  h2_tiga_sleep_enter_finish();
  vTaskDelete(NULL);
}

/* Serialized owner: reserve before hardware preparation, then commit or cancel
 * from that same caller. Allocation failure has no GPIO/peripheral effects.
 * An internal-stack terminal supervisor needs no allocation. */
static inline esp_err_t h2_tiga_sleep_prepare_deep_sleep(void) {
  volatile char stack_probe = 0;
  if (esp_ptr_internal((const void*)&stack_probe)) return ESP_OK;
  if (!h2_tiga_sleep_enter_claim()) return ESP_ERR_INVALID_STATE;
  if (xTaskCreate(h2_tiga_sleep_enter_task, H2_TIGA_SLEEP_ENTER_TASK_NAME,
                  H2_TIGA_SLEEP_ENTER_TASK_STACK, NULL,
                  H2_TIGA_SLEEP_ENTER_TASK_PRIORITY, NULL) != pdPASS) {
    h2_tiga_sleep_enter_finish();
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

static inline void h2_tiga_sleep_cancel_prepared(void) {
  volatile char stack_probe = 0;
  if (esp_ptr_internal((const void*)&stack_probe)) return;
  portENTER_CRITICAL(&h2_tiga_sleep_enter_lock);
  if (h2_tiga_sleep_enter_state == H2_TIGA_SLEEP_ENTER_RESERVED ||
      h2_tiga_sleep_enter_state == H2_TIGA_SLEEP_ENTER_QUEUED)
    h2_tiga_sleep_enter_state = H2_TIGA_SLEEP_ENTER_CANCELLED;
  portEXIT_CRITICAL(&h2_tiga_sleep_enter_lock);
}

/* A refused/stuck commit retains the in-flight helper until it reports back.
 * Never delete its active stack. Internal terminal callers may still sleep. */
static inline esp_err_t h2_tiga_sleep_enter_deep_sleep(void) {
  volatile char stack_probe = 0;
  if (esp_ptr_internal((const void*)&stack_probe)) {
    esp_deep_sleep_start();
    return ESP_ERR_INVALID_STATE;
  }
  if (h2_tiga_sleep_enter_get_state() != H2_TIGA_SLEEP_ENTER_RESERVED) {
    esp_err_t rc = h2_tiga_sleep_prepare_deep_sleep();
    if (rc != ESP_OK) return rc;
  }
  portENTER_CRITICAL(&h2_tiga_sleep_enter_lock);
  bool ready = h2_tiga_sleep_enter_state == H2_TIGA_SLEEP_ENTER_RESERVED;
  if (ready) h2_tiga_sleep_enter_state = H2_TIGA_SLEEP_ENTER_QUEUED;
  portEXIT_CRITICAL(&h2_tiga_sleep_enter_lock);
  if (!ready) return ESP_ERR_INVALID_STATE;
  for (uint32_t waited = 0; waited < H2_TIGA_SLEEP_ENTER_TIMEOUT_MS;
       waited += H2_TIGA_SLEEP_ENTER_POLL_MS) {
    if (h2_tiga_sleep_enter_get_state() == H2_TIGA_SLEEP_ENTER_RETURNED)
      return ESP_ERR_INVALID_STATE;
    vTaskDelay(pdMS_TO_TICKS(H2_TIGA_SLEEP_ENTER_POLL_MS));
  }
  if (h2_tiga_sleep_enter_get_state() == H2_TIGA_SLEEP_ENTER_RETURNED)
    return ESP_ERR_INVALID_STATE;
  /* A queued helper can still be cancelled atomically against its SDK entry.
   * Once the SDK has started, this terminal operation cannot return a recovery
   * error while that call may still sleep later. Retain ownership until reset
   * or an actual SDK return; the internal supervisor can still override it. */
  h2_tiga_sleep_cancel_prepared();
  while (h2_tiga_sleep_enter_get_state() == H2_TIGA_SLEEP_ENTER_IN_FLIGHT)
    vTaskDelay(pdMS_TO_TICKS(H2_TIGA_SLEEP_ENTER_POLL_MS));
  return h2_tiga_sleep_enter_get_state() == H2_TIGA_SLEEP_ENTER_RETURNED
             ? ESP_ERR_INVALID_STATE
             : ESP_ERR_TIMEOUT;
}

#endif
