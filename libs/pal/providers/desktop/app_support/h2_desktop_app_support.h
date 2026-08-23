#ifndef H2_DESKTOP_APP_SUPPORT_H
#define H2_DESKTOP_APP_SUPPORT_H

#include "h2_desktop_platform.h"
#include "h2_portaudio.h"
#include "h2_sdl3.h"
#include "h2_sqlite.h"
#include "h2/pal/net/h2_pal_netif.h"
#include "h2/pal/os/h2_pal_serial_host.h"
#include "h2_runtime.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace h2::desktop {

enum class ProcessState {
  kRunning = 1,
  kStopping,
  kFailed,
};

struct StatusSurface;

struct FilesystemMount {
  const char *source;
  const char *target;
};

struct Layout {
  const char *app_name;
  const char *title;
  int width;
  int height;
  const FilesystemMount *mounts;
  std::size_t mount_count;
  const h2_desktop_peripheral_config_t *peripherals;
  std::size_t peripheral_count;
  const char *normalized_json;
};

struct OwnedFilesystem {
  void *handle = nullptr;
  std::vector<std::string> sources;
  std::vector<const char *> source_paths;
  std::vector<const char *> targets;

  OwnedFilesystem() = default;
  OwnedFilesystem(const OwnedFilesystem &) = delete;
  OwnedFilesystem &operator=(const OwnedFilesystem &) = delete;
  ~OwnedFilesystem();

  const h2_pal_fs_api_t *api() const;
};

struct OwnedPreference {
  h2_sqlite_t *handle = nullptr;

  OwnedPreference() = default;
  OwnedPreference(const OwnedPreference &) = delete;
  OwnedPreference &operator=(const OwnedPreference &) = delete;
  ~OwnedPreference();

  const h2_pal_pref_api_t *api() const;
};

struct OwnedAudio {
  h2_portaudio_t *handle = nullptr;

  OwnedAudio() = default;
  OwnedAudio(const OwnedAudio &) = delete;
  OwnedAudio &operator=(const OwnedAudio &) = delete;
  ~OwnedAudio();

  h2_pal_audio_t *api() const;
};

struct OwnedDisplay {
  h2_sdl3_t *handle = nullptr;

  OwnedDisplay() = default;
  OwnedDisplay(const OwnedDisplay &) = delete;
  OwnedDisplay &operator=(const OwnedDisplay &) = delete;
  ~OwnedDisplay();

  h2_pal_display_t *display() const;
  const h2_pal_touch_api_t *touch() const;
};

struct OwnedNetworkServices {
  bool wolfssl_owner = false;
  void *mqtt_handle = nullptr;
  h2_pal_mqtt_api_t mqtt_view = {};
  void *sctp_handle = nullptr;
  void *peer_handle = nullptr;

  OwnedNetworkServices() = default;
  OwnedNetworkServices(const OwnedNetworkServices &) = delete;
  OwnedNetworkServices &operator=(const OwnedNetworkServices &) = delete;
  ~OwnedNetworkServices();

  const h2_pal_crypto_api_t *crypto() const;
  const h2_pal_dtls_api_t *dtls() const;
  const h2_pal_mqtt_api_t *mqtt() const;
  const h2_pal_webrtc_api_t *webrtc() const;
};

struct OwnedLvgl {
  void *display_adapter = nullptr;
  void *touch_adapter = nullptr;
  void *input_bridge = nullptr;

  OwnedLvgl() = default;
  OwnedLvgl(const OwnedLvgl &) = delete;
  OwnedLvgl &operator=(const OwnedLvgl &) = delete;
  ~OwnedLvgl();

  void *display() const;
  void *keyboard() const;
  void *wheel() const;
};

struct OwnedStatusSurface {
  StatusSurface *handle = nullptr;

  OwnedStatusSurface() = default;
  OwnedStatusSurface(const OwnedStatusSurface &) = delete;
  OwnedStatusSurface &operator=(const OwnedStatusSurface &) = delete;
  ~OwnedStatusSurface();
};

int configure_layout(const Layout &layout);
int open_filesystem(const Layout &layout, OwnedFilesystem *filesystem);
int open_preference(OwnedPreference *preference);
int open_audio(bool require_real_devices, OwnedAudio *audio);
int open_display(const Layout &layout, OwnedDisplay *display);
int open_network_services(bool with_mqtt, bool with_webrtc,
                          OwnedNetworkServices *services);
int open_lvgl(OwnedDisplay *display, OwnedLvgl *lvgl);
int open_status_surface(OwnedDisplay *display, const char *app_name,
                        OwnedStatusSurface *surface);
int set_process_state(OwnedStatusSurface *surface, ProcessState state);
std::string resource_path(const char *relative);
bool load_resource(const char *relative,
                   std::vector<std::uint8_t> *output);
bool load_file(const h2_pal_fs_api_t *fs, const char *path,
               std::vector<std::uint8_t> *output);
int poll_events(OwnedDisplay *display, OwnedLvgl *lvgl = nullptr);
int read_pointer(OwnedDisplay *display, h2_sdl3_pointer_state_t *out_state);
void wait_for_close(OwnedDisplay *display, std::uint32_t interval_ms = 16u);
const h2_pal_netif_api_t *netif_api();
const h2_pal_net_api_t *host_net_api();
const h2_pal_system_event_api_t *system_event_api();
const h2_pal_serial_host_api_t *serial_host_api();
const h2_pal_ble_host_api_t *corebluetooth_api();
h2_runtime_config_t runtime_config(const h2_pal_fs_api_t *fs);

} // namespace h2::desktop

#endif // H2_DESKTOP_APP_SUPPORT_H
