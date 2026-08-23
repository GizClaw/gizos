#include "h2_desktop_app_support.h"

#include "h2_ffmpeg.h"
#include "h2_coremqtt.h"
#include "h2_peer.h"
#include "h2_sctp.h"
#include "h2_wolfssl.h"
#include "h2_desktop_lvgl_input.h"
#include "h2_lvgl_display.h"
#include "h2_lvgl_touch.h"

#include "h2/pal/h2_pal_unsupported.h"

#if defined(__APPLE__)
#include "h2_darwin_platform.h"
#elif defined(__linux__)
#include "h2_linux_platform.h"
#include "h2_linux_serial_host.h"
#else
#error "Desktop app support requires Linux or macOS"
#endif

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

namespace h2::desktop {
namespace {

void reset_network_services(OwnedNetworkServices *services) {
  auto *peer = static_cast<h2_peer_t *>(services->peer_handle);
  h2_peer_destroy(&peer);
  services->peer_handle = nullptr;
  auto *sctp = static_cast<h2_sctp_t *>(services->sctp_handle);
  (void)h2_sctp_destroy(&sctp);
  services->sctp_handle = nullptr;
  h2_coremqtt_destroy(static_cast<h2_coremqtt_t *>(services->mqtt_handle));
  services->mqtt_handle = nullptr;
  services->mqtt_view = {};
  if (services->wolfssl_owner) {
    (void)h2_wolfssl_deinit();
    services->wolfssl_owner = false;
  }
}

void reset_lvgl(OwnedLvgl *lvgl) {
  h2_desktop_lvgl_input_destroy(
      static_cast<H2DesktopLvglInput *>(lvgl->input_bridge));
  lvgl->input_bridge = nullptr;
  h2_lvgl_touch_destroy(static_cast<h2_lvgl_touch_t *>(lvgl->touch_adapter));
  lvgl->touch_adapter = nullptr;
  h2_lvgl_display_destroy(
      static_cast<h2_lvgl_display_t *>(lvgl->display_adapter));
  lvgl->display_adapter = nullptr;
}

std::filesystem::path runfiles_root() {
  if (const char *resource_root =
          std::getenv("H2_DESKTOP_RESOURCE_ROOT");
      resource_root != nullptr && resource_root[0] != '\0') {
    return resource_root;
  }
  if (const char *workspace =
          std::getenv("BUILD_WORKSPACE_DIRECTORY");
      workspace != nullptr && workspace[0] != '\0') {
    return workspace;
  }
  if (const char *runfiles = std::getenv("RUNFILES_DIR");
      runfiles != nullptr && runfiles[0] != '\0') {
    for (const char *workspace_name : {"_main", "firmwares"}) {
      std::filesystem::path candidate =
          std::filesystem::path(runfiles) / workspace_name;
      std::error_code error;
      if (std::filesystem::is_directory(candidate, error)) {
        return candidate;
      }
    }
  }
  return std::filesystem::current_path();
}

} // namespace

OwnedFilesystem::~OwnedFilesystem() {
#if defined(__APPLE__)
  h2_darwin_host_fs_destroy(static_cast<h2_darwin_host_fs_t *>(handle));
#else
  h2_linux_host_fs_destroy(static_cast<h2_linux_host_fs_t *>(handle));
#endif
}

const h2_pal_fs_api_t *OwnedFilesystem::api() const {
#if defined(__APPLE__)
  return h2_darwin_host_fs_api(static_cast<h2_darwin_host_fs_t *>(handle));
#else
  return h2_linux_host_fs_api(static_cast<h2_linux_host_fs_t *>(handle));
#endif
}

OwnedPreference::~OwnedPreference() { h2_sqlite_destroy(handle); }

const h2_pal_pref_api_t *OwnedPreference::api() const {
  return h2_sqlite_pref_api(handle);
}

OwnedAudio::~OwnedAudio() { h2_portaudio_destroy(handle); }

h2_pal_audio_t *OwnedAudio::api() const { return h2_portaudio_audio(handle); }

OwnedDisplay::~OwnedDisplay() { h2_sdl3_destroy(handle); }

h2_pal_display_t *OwnedDisplay::display() const {
  return h2_sdl3_display(handle);
}

const h2_pal_touch_api_t *OwnedDisplay::touch() const {
  return h2_sdl3_touch(handle);
}

OwnedNetworkServices::~OwnedNetworkServices() { reset_network_services(this); }

const h2_pal_crypto_api_t *OwnedNetworkServices::crypto() const {
  return wolfssl_owner ? h2_wolfssl_crypto_api()
                       : h2_pal_unsupported_crypto_api();
}

const h2_pal_dtls_api_t *OwnedNetworkServices::dtls() const {
  return wolfssl_owner ? h2_wolfssl_dtls_api()
                       : h2_pal_unsupported_dtls_api();
}

const h2_pal_mqtt_api_t *OwnedNetworkServices::mqtt() const {
  return mqtt_handle == nullptr ? h2_pal_unsupported_mqtt_api() : &mqtt_view;
}

const h2_pal_webrtc_api_t *OwnedNetworkServices::webrtc() const {
  return peer_handle == nullptr
             ? h2_pal_unsupported_webrtc_api()
             : h2_peer_webrtc_api(static_cast<h2_peer_t *>(peer_handle));
}

OwnedLvgl::~OwnedLvgl() { reset_lvgl(this); }

void *OwnedLvgl::display() const {
  return h2_lvgl_display_lvgl(
      static_cast<h2_lvgl_display_t *>(display_adapter));
}

void *OwnedLvgl::keyboard() const {
  return h2_desktop_lvgl_input_keyboard(
      static_cast<H2DesktopLvglInput *>(input_bridge));
}

void *OwnedLvgl::wheel() const {
  return h2_desktop_lvgl_input_wheel(
      static_cast<H2DesktopLvglInput *>(input_bridge));
}

int configure_layout(const Layout &layout) {
  return h2_desktop_platform_configure_peripherals(
      layout.peripherals, layout.peripheral_count);
}

int open_filesystem(const Layout &layout, OwnedFilesystem *filesystem) {
  if (filesystem == nullptr || filesystem->handle != nullptr ||
      (layout.mounts == nullptr && layout.mount_count != 0u)) {
    return H2_PAL_FS_ERR_INVALID_ARG;
  }
  filesystem->sources.clear();
  filesystem->source_paths.clear();
  filesystem->targets.clear();
  try {
    filesystem->sources.reserve(layout.mount_count);
    filesystem->source_paths.reserve(layout.mount_count);
    filesystem->targets.reserve(layout.mount_count);
    for (std::size_t index = 0; index < layout.mount_count; ++index) {
      filesystem->sources.push_back(
          resource_path(layout.mounts[index].source));
    }
    for (std::size_t index = 0; index < layout.mount_count; ++index) {
      filesystem->source_paths.push_back(filesystem->sources[index].c_str());
      filesystem->targets.push_back(layout.mounts[index].target);
    }
  } catch (...) {
    filesystem->sources.clear();
    filesystem->source_paths.clear();
    filesystem->targets.clear();
    return H2_PAL_FS_ERR_NO_MEMORY;
  }
#if defined(__APPLE__)
  h2_darwin_host_fs_t *handle = nullptr;
  const int result = h2_darwin_host_fs_create(
      filesystem->source_paths.empty() ? nullptr
                                       : filesystem->source_paths.data(),
      filesystem->targets.empty() ? nullptr : filesystem->targets.data(),
      filesystem->targets.size(), &handle);
#else
  h2_linux_host_fs_t *handle = nullptr;
  const int result = h2_linux_host_fs_create(
      filesystem->source_paths.empty() ? nullptr
                                       : filesystem->source_paths.data(),
      filesystem->targets.empty() ? nullptr : filesystem->targets.data(),
      filesystem->targets.size(), &handle);
#endif
  filesystem->handle = handle;
  return result;
}

int open_preference(OwnedPreference *preference) {
  if (preference == nullptr || preference->handle != nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::string value;
  try {
    std::filesystem::path path = "prefs.sqlite";
    if (const char *root = std::getenv("H2_DESKTOP_STORAGE_ROOT");
        root != nullptr && root[0] != '\0') {
      path = std::filesystem::path(root) / "prefs.sqlite";
    }
    value = path.lexically_normal().string();
  } catch (...) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  const h2_sqlite_config_t config = {value.c_str()};
  return h2_sqlite_create(&config, &preference->handle);
}

int open_audio(bool require_real_devices, OwnedAudio *audio) {
  if (audio == nullptr || audio->handle != nullptr) {
    return H2_AUDIO_ERR_INVALID_ARG;
  }
  const h2_portaudio_config_t config = {
      h2_desktop_platform_default_allocator(),
      h2_desktop_platform_queue_api(),
      h2_desktop_platform_sync_api(),
      require_real_devices ? 1 : 0,
  };
  return h2_portaudio_create(&config, &audio->handle);
}

int open_display(const Layout &layout, OwnedDisplay *display) {
  if (display == nullptr || display->handle != nullptr) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  const h2_sdl3_config_t config = {layout.title, layout.width, layout.height};
  return h2_sdl3_create(&config, &display->handle);
}

int open_network_services(bool with_mqtt, bool with_webrtc,
                          OwnedNetworkServices *services) {
  if (services == nullptr || services->wolfssl_owner ||
      services->mqtt_handle != nullptr || services->sctp_handle != nullptr ||
      services->peer_handle != nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_wolfssl_config_t wolfssl = {
      *h2_desktop_platform_default_allocator(),
      nullptr,
#if defined(__APPLE__)
      h2_darwin_entropy,
#else
      h2_linux_entropy,
#endif
  };
  h2_pal_result_t result = h2_wolfssl_init(&wolfssl);
  if (result != H2_PAL_OK) {
    return result;
  }
  services->wolfssl_owner = true;
  if (with_mqtt) {
    const h2_coremqtt_config_t mqtt = {
        h2_desktop_platform_default_allocator(),
        host_net_api(),
        h2_desktop_platform_time_api(),
        h2_desktop_platform_log_api(),
        8u,
        8u,
    };
    h2_coremqtt_t *provider = nullptr;
    result = h2_coremqtt_create(&mqtt, &provider, &services->mqtt_view);
    services->mqtt_handle = provider;
  }
  if (result == H2_PAL_OK && with_webrtc) {
    const h2_sctp_config_t sctp = {
        h2_desktop_platform_default_allocator(), services->crypto()};
    h2_sctp_t *provider = nullptr;
    result = h2_sctp_create(&sctp, &provider);
    services->sctp_handle = provider;
    if (result == H2_PAL_OK) {
      const h2_peer_config_t peer = {
          h2_desktop_platform_default_allocator(),
          h2_desktop_platform_default_allocator(),
          h2_desktop_platform_log_api(),
          host_net_api(),
          h2_desktop_platform_queue_api(),
          h2_desktop_platform_sync_api(),
          h2_desktop_platform_task_api(),
          h2_desktop_platform_time_api(),
          services->crypto(),
          services->dtls(),
          h2_sctp_api(provider),
      };
      h2_peer_t *peer_provider = nullptr;
      result = h2_peer_create(&peer, &peer_provider);
      services->peer_handle = peer_provider;
    }
  }
  if (result != H2_PAL_OK) {
    reset_network_services(services);
  }
  return result;
}

int open_lvgl(OwnedDisplay *display, OwnedLvgl *lvgl) {
  if (display == nullptr || display->handle == nullptr || lvgl == nullptr ||
      lvgl->display_adapter != nullptr || lvgl->touch_adapter != nullptr ||
      lvgl->input_bridge != nullptr || !lv_is_initialized()) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const h2_lvgl_display_config_t display_config = {
      display->display(), h2_desktop_platform_default_allocator(), 32u};
  h2_lvgl_display_t *display_adapter = nullptr;
  h2_pal_result_t result =
      h2_lvgl_display_create(&display_config, &display_adapter);
  lvgl->display_adapter = display_adapter;
  if (result == H2_PAL_OK) {
    const h2_lvgl_touch_config_t touch_config = {
        display->touch(), h2_desktop_platform_default_allocator(),
        h2_lvgl_display_lvgl(display_adapter)};
    h2_lvgl_touch_t *touch_adapter = nullptr;
    result = h2_lvgl_touch_create(&touch_config, &touch_adapter);
    lvgl->touch_adapter = touch_adapter;
  }
  if (result == H2_PAL_OK) {
    H2DesktopLvglInput *input = nullptr;
    result = static_cast<h2_pal_result_t>(h2_desktop_lvgl_input_create(
        static_cast<lv_display_t *>(lvgl->display()), &input));
    lvgl->input_bridge = input;
  }
  if (result != H2_PAL_OK) {
    reset_lvgl(lvgl);
  }
  return result;
}

std::string resource_path(const char *relative) {
  if (relative == nullptr || relative[0] == '\0') {
    return {};
  }
  const std::filesystem::path path =
      runfiles_root() / std::filesystem::path(relative);
  return path.lexically_normal().string();
}

bool load_resource(const char *relative,
                   std::vector<std::uint8_t> *output) {
  if (output == nullptr) {
    return false;
  }
  std::ifstream stream(resource_path(relative), std::ios::binary);
  if (!stream) {
    return false;
  }
  try {
    output->assign(std::istreambuf_iterator<char>(stream),
                   std::istreambuf_iterator<char>());
  } catch (...) {
    output->clear();
    return false;
  }
  return !stream.bad() && !output->empty();
}

bool load_file(const h2_pal_fs_api_t *fs, const char *path,
               std::vector<std::uint8_t> *output) {
  if (fs == nullptr || path == nullptr || output == nullptr) {
    return false;
  }
  h2_pal_fs_file_t *file = nullptr;
  if (h2_pal_fs_open(fs, path, H2_PAL_FS_OPEN_READ, &file) !=
          H2_PAL_FS_OK ||
      file == nullptr) {
    return false;
  }
  h2_pal_fs_stat_t stat = {};
  if (h2_pal_fs_stat(fs, path, &stat) != H2_PAL_FS_OK ||
      stat.size > SIZE_MAX) {
    (void)h2_pal_fs_close(fs, file);
    return false;
  }
  try {
    output->resize(static_cast<std::size_t>(stat.size));
  } catch (...) {
    (void)h2_pal_fs_close(fs, file);
    return false;
  }
  std::size_t total = 0u;
  while (total < output->size()) {
    std::size_t received = 0u;
    if (h2_pal_fs_read(fs, file, output->data() + total,
                       output->size() - total, &received) !=
            H2_PAL_FS_OK ||
        received == 0u) {
      (void)h2_pal_fs_close(fs, file);
      return false;
    }
    total += received;
  }
  return h2_pal_fs_close(fs, file) == H2_PAL_FS_OK;
}

int poll_events(OwnedDisplay *display, OwnedLvgl *lvgl) {
  if (display == nullptr || display->handle == nullptr) {
    return 1;
  }
  h2_sdl3_event_t event = {};
  while (h2_sdl3_poll_event(display->handle, &event) == H2_PAL_OK) {
    if (lvgl != nullptr && lvgl->input_bridge != nullptr) {
      h2_desktop_lvgl_input_handle(
          static_cast<H2DesktopLvglInput *>(lvgl->input_bridge), event);
    }
    switch (event.kind) {
    case H2_SDL3_EVENT_CLOSE:
      return 1;
    case H2_SDL3_EVENT_FOCUS_LOST:
      h2_desktop_platform_handle_focus_lost();
      break;
    case H2_SDL3_EVENT_KEY:
      if (event.key >= H2_SDL3_KEY_SPACE &&
          event.key <= H2_SDL3_KEY_DIGIT_9) {
        (void)h2_desktop_platform_handle_key(
            static_cast<h2_desktop_key_t>(event.key), event.pressed,
            event.repeat);
      }
      break;
    case H2_SDL3_EVENT_TEXT:
    case H2_SDL3_EVENT_WHEEL:
      break;
    }
  }
  return 0;
}

int read_pointer(OwnedDisplay *display, h2_sdl3_pointer_state_t *out_state) {
  return display == nullptr
             ? H2_PAL_ERR_INVALID_ARG
             : h2_sdl3_read_pointer(display->handle, out_state);
}

void wait_for_close(OwnedDisplay *display, std::uint32_t interval_ms) {
  while (poll_events(display) == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
}

const h2_pal_netif_api_t *netif_api() {
#if defined(__APPLE__)
  return h2_darwin_netif_api();
#else
  return h2_linux_netif_api();
#endif
}

const h2_pal_net_api_t *host_net_api() {
#if defined(__APPLE__)
  return h2_darwin_net_api();
#else
  return h2_linux_net_api();
#endif
}

const h2_pal_system_event_api_t *system_event_api() {
#if defined(__APPLE__)
  return h2_darwin_system_event_api();
#else
  return h2_linux_system_event_api();
#endif
}

const h2_pal_serial_host_api_t *serial_host_api() {
#if defined(__APPLE__)
  return h2_darwin_serial_host_api();
#else
  return h2_linux_serial_host_api();
#endif
}

const h2_pal_ble_host_api_t *corebluetooth_api() {
#if defined(__APPLE__)
  return h2_darwin_corebluetooth_ble(
      h2_desktop_platform_default_allocator());
#else
  return h2_pal_unsupported_ble_host_api();
#endif
}

h2_runtime_config_t runtime_config(const h2_pal_fs_api_t *fs) {
  h2_runtime_config_t config = {};
  config.board = "desktop";
  config.target = "host";
  config.chip = "host";
  config.firmware_info = h2_pal_unsupported_firmware_info_api();
  config.mem = h2_desktop_platform_default_allocator();
  config.log = h2_desktop_platform_log_api();
  config.time = h2_desktop_platform_time_api();
  config.timer = h2_pal_unsupported_timer_api();
  config.task = h2_desktop_platform_task_api();
  config.queue = h2_desktop_platform_queue_api();
  config.sync = h2_desktop_platform_sync_api();
  config.fs = fs == nullptr ? h2_pal_unsupported_fs_api() : fs;
  config.disk = h2_pal_unsupported_disk_api();
  config.pref = h2_pal_unsupported_pref_api();
  config.crypto = h2_pal_unsupported_crypto_api();
  config.http = h2_pal_unsupported_http_api();
  config.net = h2_pal_unsupported_net_api();
  config.netif = netif_api();
  config.mqtt = h2_pal_unsupported_mqtt_api();
  config.webrtc = h2_pal_unsupported_webrtc_api();
  config.wifi_sta = h2_pal_unsupported_wifi_sta_api();
  config.wifi_ap = h2_pal_unsupported_wifi_ap_api();
  config.wifi_csi = h2_pal_unsupported_wifi_csi_api();
  config.wifi_settings = h2_pal_unsupported_wifi_settings_api();
  config.ble_host = h2_pal_unsupported_ble_host_api();
  config.modem = h2_pal_unsupported_modem_api();
  config.power = h2_pal_unsupported_power_api();
  config.display = h2_pal_unsupported_display_api();
  config.audio = h2_pal_unsupported_audio_api();
  config.audio_decoder = h2_ffmpeg_audio_decoder_api();
  config.periph = h2_desktop_platform_periph_api();
  config.button = h2_desktop_platform_button_api();
  config.touch = h2_pal_unsupported_touch_api();
  config.buzzer = h2_pal_unsupported_buzzer_api();
  config.nfc = h2_desktop_platform_nfc_api();
  config.nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api();
  config.imu = h2_desktop_platform_imu_api();
  config.gpio_irq = h2_pal_unsupported_gpio_irq_api();
  config.led = h2_pal_unsupported_led_api();
  config.switch_api = h2_pal_unsupported_switch_api();
  config.pwm_switch = h2_pal_unsupported_pwm_switch_api();
  config.input = h2_pal_unsupported_input_api();
  config.system_event = system_event_api();
  config.video_decoder = h2_ffmpeg_video_decoder_api();
  config.component_mapper = nullptr;
  config.event_queue_capacity =
      H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY;
  return config;
}

} // namespace h2::desktop
