#include "esp_loader.h"
#include "h2_h2loader_host_internal.h"
#include <assert.h>
#include <stddef.h>

int main(void) {
  assert(h2_h2loader_host_esp_expected_chip("esp32c3") == ESP32C3_CHIP);
  assert(h2_h2loader_host_esp_expected_chip("esp32s3") == ESP32S3_CHIP);
  assert(h2_h2loader_host_esp_expected_chip("esp32p4") == ESP32P4_CHIP);
  assert(h2_h2loader_host_esp_expected_chip("esp32c5") == ESP32C5_CHIP);
  assert(h2_h2loader_host_esp_expected_chip("esp32") == ESP_UNKNOWN_CHIP);
  assert(h2_h2loader_host_esp_expected_chip("ESP32C3") == ESP_UNKNOWN_CHIP);
  assert(h2_h2loader_host_esp_expected_chip(NULL) == ESP_UNKNOWN_CHIP);
  return 0;
}
