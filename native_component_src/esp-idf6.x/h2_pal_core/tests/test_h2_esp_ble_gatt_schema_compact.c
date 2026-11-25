#include "h2_esp_ble_gatt_schema.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

int main(void) {
  assert(H2_ESP_BLE_MAX_GATT_SERVICES == 1);
  assert(H2_ESP_BLE_MAX_GATT_CHARACTERISTICS == 2);
  assert(h2_esp_ble_gatt_schema_accepts(1, 2));
  assert(!h2_esp_ble_gatt_schema_accepts(2, 2));
  assert(!h2_esp_ble_gatt_schema_accepts(1, 3));
  uint8_t guarded[] = {0xee, 0xff, 0xff, 0xee};
  h2_esp_ble_gatt_schema_bind_indices(guarded + 1, 0, 2);
  assert(guarded[0] == 0xee && guarded[3] == 0xee);
  assert(guarded[1] == 0 && guarded[2] == 1);
  return 0;
}
