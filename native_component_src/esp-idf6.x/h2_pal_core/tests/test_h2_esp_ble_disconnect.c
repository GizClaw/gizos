#include "h2/pal/hal/h2_pal_ble.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

enum { BLE_HS_ENOTCONN = 7, BLE_ERR_REM_USER_CONN_TERM = 19 };
static int terminate_result;
static unsigned terminate_calls;

static int ble_gap_terminate(uint16_t handle, uint8_t reason) {
  assert(handle == 7u && reason == BLE_ERR_REM_USER_CONN_TERM);
  ++terminate_calls;
  return terminate_result;
}

static h2_pal_result_t h2_esp_ble_map_rc(int rc) {
  return rc == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

#include "tests/h2_esp_ble_disconnect_for_test.inc"

int main(void) {
  h2_pal_ble_t ble = {0};
  assert(h2_esp_ble_disconnect(&ble, 7u) == H2_PAL_OK);
  terminate_result = BLE_HS_ENOTCONN;
  assert(h2_esp_ble_disconnect(&ble, 7u) == H2_PAL_ERR_CLOSED);
  assert(h2_esp_ble_disconnect(&ble, 7u) == H2_PAL_ERR_CLOSED);
  terminate_result = 42;
  assert(h2_esp_ble_disconnect(&ble, 7u) == H2_PAL_ERR_IO);
  assert(terminate_calls == 4u);
  return 0;
}
