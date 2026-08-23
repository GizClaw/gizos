#include "h2_desktop_app_support.h"
#include "h2_smoke_audio_system.h"
#include "layout_config.h"

#include <cstdio>

namespace {

constexpr h2::desktop::Layout kLayout = {
    h2_desktop_layout::app_name,
    h2_desktop_layout::title,
    h2_desktop_layout::width,
    h2_desktop_layout::height,
    h2_desktop_layout::mounts,
    h2_desktop_layout::mount_count,
    h2_desktop_layout::peripherals,
    h2_desktop_layout::peripheral_count,
    h2_desktop_layout::normalized_json,
};

} // namespace

int main() {
  h2::desktop::OwnedAudio audio;
  h2::desktop::OwnedDisplay display;
  h2::desktop::OwnedStatusSurface status;
  if (h2::desktop::configure_layout(kLayout) != H2_PAL_OK ||
      h2::desktop::open_display(kLayout, &display) != H2_DISPLAY_OK ||
      h2::desktop::open_audio(true, &audio) != H2_AUDIO_OK ||
      h2::desktop::open_status_surface(
          &display, "example/audio-system", &status) != H2_DISPLAY_OK) {
    std::fprintf(stderr, "desktop %s: provider initialization failed\n",
                 kLayout.app_name);
    return 1;
  }

  h2::desktop::OwnedFilesystem filesystem;
  const int fs_result =
      h2::desktop::open_filesystem(kLayout, &filesystem);
  if (fs_result != H2_PAL_FS_OK || filesystem.handle == nullptr) {
    std::fprintf(stderr, "desktop %s: filesystem init failed (%d)\n",
                 kLayout.app_name, fs_result);
    (void)h2::desktop::set_process_state(
        &status, h2::desktop::ProcessState::kFailed);
    h2::desktop::wait_for_close(&display);
    return 1;
  }

  h2_runtime_config_t config = h2::desktop::runtime_config(
      filesystem.api());
  config.audio = audio.api();
  h2_runtime_t *runtime = nullptr;
  const int runtime_result = h2_runtime_init(&config, &runtime);
  if (runtime_result != H2_PAL_OK || runtime == nullptr) {
    std::fprintf(stderr, "desktop %s: runtime init failed (%d)\n",
                 kLayout.app_name, runtime_result);
    (void)h2::desktop::set_process_state(
        &status, h2::desktop::ProcessState::kFailed);
    h2::desktop::wait_for_close(&display);
    return 1;
  }

  const h2_smoke_audio_system_config_t app_config = {
      "/data/audio/music_loop.ogg",
      0u,
  };
  const int app_result =
      h2_smoke_audio_system_run(runtime, &app_config);
  int stop_result = H2_AUDIO_OK;
  if (app_result == H2_AUDIO_OK) {
    (void)h2::desktop::set_process_state(
        &status, h2::desktop::ProcessState::kRunning);
  } else {
    std::fprintf(
        stderr,
        "desktop %s: app failed (%d); music_path=%s; real PortAudio "
        "devices required\n",
        kLayout.app_name, app_result, app_config.music_path);
    stop_result = h2_smoke_audio_system_stop();
    (void)h2::desktop::set_process_state(
        &status, h2::desktop::ProcessState::kFailed);
  }
  h2::desktop::wait_for_close(&display);
  if (app_result == H2_AUDIO_OK) {
    (void)h2::desktop::set_process_state(
        &status, h2::desktop::ProcessState::kStopping);
    (void)h2::desktop::poll_events(&display);
    stop_result = h2_smoke_audio_system_stop();
  }
  if (stop_result != H2_AUDIO_OK) {
    std::fprintf(stderr, "desktop %s: app shutdown failed (%d)\n",
                 kLayout.app_name, stop_result);
    return 1;
  }
  h2_runtime_deinit(runtime);
  return app_result == H2_AUDIO_OK ? 0 : 1;
}
