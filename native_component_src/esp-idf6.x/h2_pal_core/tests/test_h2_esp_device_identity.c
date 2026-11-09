#include "h2_esp_platform_core.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "sdkconfig.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static unsigned factory_calls, bluetooth_calls;
static esp_err_t mac_result;
static const uint8_t factory_mac[6] = {0xe8, 0xf6, 0x0a, 0xe0, 0xb3, 0xcd};
static const uint8_t bluetooth_mac[6] = {0x30, 0xed, 0xa0, 0xae, 0x0f, 0x86};

esp_err_t esp_efuse_mac_get_default(uint8_t* mac) {
  ++factory_calls;
  memcpy(mac, factory_mac, sizeof(factory_mac));
  return mac_result;
}

esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type) {
  assert(type == ESP_MAC_BT);
  ++bluetooth_calls;
  memcpy(mac, bluetooth_mac, sizeof(bluetooth_mac));
  return mac_result;
}

const esp_app_desc_t* esp_app_get_description(void) {
  static const esp_app_desc_t description = {.version = "test-version"};
  return &description;
}

int main(void) {
  const h2_pal_device_identity_api_t* api =
      h2_esp_platform_device_identity_api();
  assert(api == h2_esp_platform_device_identity_api());
  assert(api->vtable->get(api->user, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(factory_calls == 0u && bluetooth_calls == 0u);
  h2_pal_device_identity_t identity;
  for (unsigned i = 0u; i < 2u; ++i) {
    memset(&identity, 0xa5, sizeof(identity));
    assert(h2_pal_device_identity_get(api, &identity) == H2_PAL_OK);
    assert(identity.size == 6u);
#if defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4
    assert(factory_calls == i + 1u && bluetooth_calls == 0u);
    assert(memcmp(identity.bytes, factory_mac, 6u) == 0);
#else
    assert(bluetooth_calls == i + 1u && factory_calls == 0u);
    assert(memcmp(identity.bytes, bluetooth_mac, 6u) == 0);
#endif
    for (size_t j = 6u; j < sizeof(identity.bytes); ++j)
      assert(identity.bytes[j] == 0u);
  }
  mac_result = ESP_ERR_INVALID_STATE;
  memset(&identity, 0xa5, sizeof(identity));
  assert(api->vtable->get(api->user, &identity) == H2_PAL_ERR_IO);
  const h2_pal_device_identity_t empty = {0};
  assert(memcmp(&identity, &empty, sizeof(identity)) == 0);
  h2_pal_firmware_info_t info;
  assert(h2_pal_firmware_info_get_current(h2_esp_platform_firmware_info_api(),
                                          &info) == H2_PAL_OK);
  assert(strcmp(info.version, "test-version") == 0);
  return 0;
}
