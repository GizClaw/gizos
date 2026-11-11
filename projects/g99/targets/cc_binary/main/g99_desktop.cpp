#include "g99_desktop.h"

#include "h2_g99.h"
#include "yyjson.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>

namespace h2::desktop::g99 {
namespace {

struct Mapping {
  h2_runtime_component_t kind;
  h2_runtime_component_id_t component_id;
  h2_pal_periph_id_t periph_id;
};

// Layout periph ids: AI and BOOT are the board keys; arrows and OK stand in
// for the touch controls and the optional joystick module.
constexpr Mapping kMappings[] = {
    {H2_RUNTIME_COMPONENT_BUTTON, H2_G99_COMPONENT_HOME, 101u},
    {H2_RUNTIME_COMPONENT_BUTTON, H2_G99_COMPONENT_SCREEN, 102u},
    {H2_RUNTIME_COMPONENT_BUTTON, H2_G99_COMPONENT_UP, 103u},
    {H2_RUNTIME_COMPONENT_BUTTON, H2_G99_COMPONENT_DOWN, 104u},
    {H2_RUNTIME_COMPONENT_BUTTON, H2_G99_COMPONENT_OK, 105u},
    {H2_RUNTIME_COMPONENT_BATTERY, H2_G99_COMPONENT_BATTERY, 401u},
};

h2_pal_result_t mapper_list(void*, h2_runtime_component_t filter,
                            h2_runtime_component_mapping_cb_t callback,
                            void* user) {
  if (callback == nullptr) return H2_PAL_ERR_INVALID_ARG;
  for (const Mapping& mapping : kMappings) {
    if (filter != H2_RUNTIME_COMPONENT_NONE && filter != mapping.kind) continue;
    const h2_runtime_component_mapping_entry_t entry = {mapping.component_id,
                                                        mapping.periph_id};
    if (callback(user, &entry) != H2_PAL_OK) return H2_PAL_ERR_IO;
  }
  return H2_PAL_OK;
}

h2_pal_result_t mapper_get(void*, h2_runtime_component_id_t component_id,
                           h2_pal_periph_id_t* out_periph_id) {
  if (out_periph_id == nullptr) return H2_PAL_ERR_INVALID_ARG;
  for (const Mapping& mapping : kMappings) {
    if (mapping.component_id == component_id) {
      *out_periph_id = mapping.periph_id;
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}

const h2_runtime_component_mapper_vtable_t kMapperVtable = {mapper_list,
                                                            mapper_get};
const h2_runtime_component_mapper_t kMapper = {nullptr, &kMapperVtable};

yyjson_val* member(yyjson_val* object, const char* name) {
  return yyjson_is_obj(object) ? yyjson_obj_get(object, name) : nullptr;
}

const char* string_of(yyjson_val* object, const char* name) {
  const char* value = yyjson_get_str(member(object, name));
  return value != nullptr ? value : "";
}

h2_pal_wifi_security_t security_of(const char* name) {
  const struct {
    const char* name;
    h2_pal_wifi_security_t value;
  } kNames[] = {
      {"open", H2_PAL_WIFI_SECURITY_OPEN},
      {"wep", H2_PAL_WIFI_SECURITY_WEP},
      {"wpa", H2_PAL_WIFI_SECURITY_WPA},
      {"wpa2", H2_PAL_WIFI_SECURITY_WPA2},
      {"wpa3", H2_PAL_WIFI_SECURITY_WPA3},
      {"wpa_wpa2", H2_PAL_WIFI_SECURITY_WPA_WPA2},
      {"wpa2_wpa3", H2_PAL_WIFI_SECURITY_WPA2_WPA3},
  };
  for (const auto& entry : kNames)
    if (std::strcmp(entry.name, name) == 0) return entry.value;
  return H2_PAL_WIFI_SECURITY_UNKNOWN;
}

// Configures the simulated station from the layout's simulation.wifi_sta.
bool configure_wifi(const char* normalized_json) {
  yyjson_doc* document =
      yyjson_read(normalized_json, std::strlen(normalized_json), 0u);
  if (document == nullptr) return false;
  yyjson_val* wifi =
      member(member(yyjson_doc_get_root(document), "simulation"), "wifi_sta");
  std::vector<h2_desktop_wifi_scan_entry_config_t> entries;
  yyjson_val* results = member(wifi, "scan_results");
  for (size_t i = 0; yyjson_is_arr(results) && i < yyjson_arr_size(results);
       ++i) {
    yyjson_val* item = yyjson_arr_get(results, i);
    const char* ssid = string_of(item, "ssid");
    const char* password = string_of(item, "password");
    entries.push_back(
        {ssid, std::strlen(ssid), password, std::strlen(password),
         static_cast<int>(yyjson_get_int(member(item, "rssi_dbm"))),
         static_cast<uint8_t>(yyjson_get_int(member(item, "channel"))),
         security_of(string_of(item, "security"))});
  }
  const char* outcome = string_of(wifi, "scan_outcome");
  const h2_desktop_wifi_sta_config_t config = {
      yyjson_get_bool(member(wifi, "connected")) ? 1 : 0,
      string_of(wifi, "ssid"),
      static_cast<int>(yyjson_get_int(member(wifi, "rssi_dbm"))),
      static_cast<uint8_t>(yyjson_get_int(member(wifi, "channel"))),
      std::strcmp(outcome, "timeout") == 0    ? H2_DESKTOP_WIFI_SCAN_TIMEOUT
      : std::strcmp(outcome, "io_error") == 0 ? H2_DESKTOP_WIFI_SCAN_IO_ERROR
                                              : H2_DESKTOP_WIFI_SCAN_SUCCESS,
      static_cast<uint32_t>(yyjson_get_uint(member(wifi, "scan_delay_ms"))),
      entries.empty() ? nullptr : entries.data(),
      entries.size(),
  };
  // The provider copies every string and entry before returning.
  const bool ok = h2_desktop_platform_configure_wifi_sta(&config) == H2_PAL_OK;
  yyjson_doc_free(document);
  return ok;
}

}  // namespace

Platform::~Platform() {
  while (close() != H2_PAL_OK)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

int Platform::open(const Layout& layout) {
  if (runtime_ != nullptr) return H2_PAL_ERR_INVALID_STATE;
  if (configure_layout(layout) != H2_PAL_OK ||
      !configure_wifi(layout.normalized_json) ||
      open_preference(&preference_) != H2_PAL_OK ||
      open_audio(false, &audio_) != H2_AUDIO_OK ||
      open_display(layout, &display_) != H2_DISPLAY_OK)
    return H2_PAL_ERR_UNAVAILABLE;
  h2_runtime_config_t config = runtime_config(nullptr);
  config.board = layout.app_name;
  config.pref = preference_.api();
  config.display = display_.display();
  config.touch = display_.touch();
  config.audio = audio_.api();
  config.wifi_sta = h2_desktop_platform_wifi_sta();
  config.wifi_settings = h2_desktop_platform_wifi_settings();
  config.input = h2_desktop_platform_input_api();
  config.component_mapper = &kMapper;
  h2_pal_result_t rc = h2_runtime_init(&config, &runtime_);
  if (rc == H2_PAL_OK) rc = h2_runtime_input_start(runtime_, nullptr);
  if (rc == H2_PAL_OK)
    rc = static_cast<h2_pal_result_t>(h2_pal_display_open(display_.display()));
  display_open_ = rc == H2_PAL_OK;
  return rc;
}

int Platform::close() {
  if (runtime_ != nullptr) {
    const int rc = h2_runtime_destroy(&runtime_);
    if (rc != H2_PAL_OK) return rc;
  }
  if (display_open_) {
    const int rc = h2_pal_display_close(display_.display());
    if (rc != H2_PAL_OK) return rc;
    display_open_ = false;
  }
  return H2_PAL_OK;
}

void use_test_storage_root() {
  const char* root = std::getenv("H2_DESKTOP_STORAGE_ROOT");
  const char* test_tmp = std::getenv("TEST_TMPDIR");
  if ((root == nullptr || root[0] == '\0') && test_tmp != nullptr &&
      test_tmp[0] != '\0')
    (void)setenv("H2_DESKTOP_STORAGE_ROOT", test_tmp, 1);
}

}  // namespace h2::desktop::g99
