#include "h2/pal/hal/h2_pal_ble.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t EventBits_t;
static bool s_h2_esp_ble_started;
static void* s_h2_esp_ble_events;
#define H2_ESP_BLE_SCAN_STOPPED_BIT 2u
#define H2_ESP_BLE_GAP_STOP_TIMEOUT_MS 6000u
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)

static int cancel_result;
static bool scanning;
static EventBits_t wait_result;
static unsigned cancel_calls;
static unsigned active_calls;
static unsigned wait_calls;
static unsigned complete_calls;

static int ble_gap_disc_cancel(void) {
  /* Calling NimBLE before initialization dereferences its absent context. */
  assert(s_h2_esp_ble_started);
  ++cancel_calls;
  return cancel_result;
}

static int ble_gap_disc_active(void) {
  assert(s_h2_esp_ble_started);
  ++active_calls;
  return scanning;
}

static void h2_esp_ble_complete_scan(void) { ++complete_calls; }

static EventBits_t xEventGroupWaitBits(void* events, EventBits_t bits,
                                       int clear, int all, uint32_t ticks) {
  assert(events == s_h2_esp_ble_events && events != NULL);
  assert(bits == H2_ESP_BLE_SCAN_STOPPED_BIT && clear && all);
  assert(ticks == H2_ESP_BLE_GAP_STOP_TIMEOUT_MS);
  ++wait_calls;
  return wait_result;
}

static h2_pal_result_t h2_esp_ble_map_rc(int rc) {
  return rc == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

#include "tests/h2_esp_ble_stop_scan_for_test.inc"

int main(void) {
  h2_pal_ble_t ble = {0};
  /* Before first start, repeated cleanup never touches NimBLE or events. */
  assert(h2_esp_ble_stop_scan(&ble) == H2_PAL_OK);
  assert(h2_esp_ble_stop_scan(&ble) == H2_PAL_OK);
  assert(cancel_calls == 0u && active_calls == 0u && wait_calls == 0u &&
         complete_calls == 0u);

  s_h2_esp_ble_started = true;
  s_h2_esp_ble_events = &ble;
  assert(h2_esp_ble_stop_scan(&ble) == H2_PAL_OK);
  assert(cancel_calls == 1u && active_calls == 1u && complete_calls == 1u &&
         wait_calls == 0u);

  /* An active scan still uses the existing bounded completion wait. */
  scanning = true;
  wait_result = H2_ESP_BLE_SCAN_STOPPED_BIT;
  assert(h2_esp_ble_stop_scan(&ble) == H2_PAL_OK);
  assert(cancel_calls == 2u && wait_calls == 1u);
  wait_result = 0u;
  assert(h2_esp_ble_stop_scan(&ble) == H2_PAL_ERR_TIMEOUT);
  assert(cancel_calls == 3u && wait_calls == 2u);
  cancel_result = 7;
  assert(h2_esp_ble_stop_scan(&ble) == H2_PAL_ERR_IO);
  assert(cancel_calls == 4u && wait_calls == 2u);

  /* After stop (or a failed start), even a retained event handle is unused. */
  s_h2_esp_ble_started = false;
  const unsigned active_before = active_calls;
  assert(h2_esp_ble_stop_scan(&ble) == H2_PAL_OK);
  assert(cancel_calls == 4u && active_calls == active_before &&
         wait_calls == 2u && complete_calls == 1u);
  return 0;
}
