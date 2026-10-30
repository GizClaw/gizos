#include "../include/h2_esp_h2loader_device_uid.h"
#include <assert.h>
#include <string.h>

static unsigned factory_calls, bluetooth_calls;
static esp_err_t result;
static const uint8_t factory_mac[6] = {0xe8, 0xf6, 0x0a, 0xe0, 0xb3, 0xcd};
static const uint8_t bluetooth_mac[6] = {0x30, 0xed, 0xa0, 0xae, 0x0f, 0x86};
esp_err_t esp_efuse_mac_get_default(uint8_t* mac) {
  ++factory_calls;
  if (result == 0) memcpy(mac, factory_mac, 6);
  return result;
}
esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type) {
  assert(type == ESP_MAC_BT);
  ++bluetooth_calls;
  if (result == 0) memcpy(mac, bluetooth_mac, 6);
  return result;
}
int main(void) {
  uint8_t mac[6] = {0};
  assert(h2_esp_h2loader_read_device_mac(mac) == 0);
#if defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4
  assert(factory_calls == 1 && bluetooth_calls == 0);
  assert(memcmp(mac, factory_mac, 6) == 0);
#else
  assert(bluetooth_calls == 1 && factory_calls == 0);
  assert(memcmp(mac, bluetooth_mac, 6) == 0);
#endif
  result = 262;
  assert(h2_esp_h2loader_read_device_mac(mac) == 262);
  assert(factory_calls + bluetooth_calls == 2);
  return 0;
}
