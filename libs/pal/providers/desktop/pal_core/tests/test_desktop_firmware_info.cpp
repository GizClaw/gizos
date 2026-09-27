#include "h2_desktop_platform.h"

#include <cassert>
#include <cstring>
#include <thread>

int main() {
  h2_desktop_platform_resource_stats_t baseline{}, stats{};
  assert(h2_desktop_platform_get_resource_stats(&baseline) == H2_PAL_OK);
  h2_desktop_firmware_info_t *owner = nullptr;
  assert(h2_desktop_firmware_info_create(nullptr, &owner) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_desktop_firmware_info_create("", &owner) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_desktop_firmware_info_create("v1", nullptr) ==
         H2_PAL_ERR_INVALID_ARG);
  char too_long[H2_PAL_FIRMWARE_VERSION_MAX + 1u];
  std::memset(too_long, 'a', sizeof(too_long));
  too_long[sizeof(too_long) - 1u] = '\0';
  assert(h2_desktop_firmware_info_create(too_long, &owner) ==
         H2_PAL_ERR_TRUNCATED);
  assert(owner == nullptr);
  too_long[H2_PAL_FIRMWARE_VERSION_MAX - 1u] = '\0';
  assert(h2_desktop_firmware_info_create(too_long, &owner) == H2_PAL_OK);
  const auto *api = h2_desktop_firmware_info_api(owner);
  assert(api != nullptr && h2_desktop_firmware_info_api(nullptr) == nullptr);
  h2_pal_firmware_info_t out{};
  assert(h2_pal_firmware_info_get_current(api, &out) == H2_PAL_OK);
  assert(std::strlen(out.version) == H2_PAL_FIRMWARE_VERSION_MAX - 1u);
  too_long[0] = 'b';
  assert(h2_pal_firmware_info_get_current(api, &out) == H2_PAL_OK);
  assert(out.version[0] == 'a'); // Owns a copy, no borrowed config string.
  assert(h2_pal_firmware_info_get_current(api, nullptr) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(api->vtable->get_current(nullptr, &out) == H2_PAL_ERR_INVALID_ARG);
  assert(out.version[0] == '\0');
  h2_desktop_firmware_info_t *second = nullptr;
  assert(h2_desktop_firmware_info_create("v2+build.123", &second) == H2_PAL_OK);
  assert(h2_desktop_platform_get_resource_stats(&stats) == H2_PAL_OK);
  assert(stats.live_firmware_infos == baseline.live_firmware_infos + 2u);
  const auto *second_api = h2_desktop_firmware_info_api(second);
  std::thread reader([&] {
    h2_pal_firmware_info_t metadata{};
    for (int i = 0; i < 1000; ++i) {
      assert(h2_pal_firmware_info_get_current(second_api, &metadata) ==
             H2_PAL_OK);
      assert(std::strcmp(metadata.version, "v2+build.123") == 0);
    }
  });
  for (int i = 0; i < 1000; ++i) {
    assert(h2_pal_firmware_info_get_current(second_api, &out) == H2_PAL_OK);
    assert(std::strcmp(out.version, "v2+build.123") == 0);
  }
  reader.join();
  h2_desktop_firmware_info_destroy(owner);
  h2_desktop_firmware_info_destroy(second);
  h2_desktop_firmware_info_destroy(nullptr);
  assert(std::strcmp(out.version, "v2+build.123") == 0); // caller-owned result
  assert(h2_desktop_platform_get_resource_stats(&stats) == H2_PAL_OK);
  assert(stats.live_firmware_infos == baseline.live_firmware_infos);
  return 0;
}
