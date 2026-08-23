#include "player.h"

#include "h2_ffmpeg.h"

#include "h2_desktop_app_support.h"
#include "h2_smoke_mp4_player.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

h2_pal_result_t read_at(void *user, std::uint64_t offset, void *buffer,
                        std::size_t capacity, std::size_t *out_read) {
  const auto *media =
      static_cast<const std::vector<std::uint8_t> *>(user);
  if (media == nullptr || out_read == nullptr ||
      (buffer == nullptr && capacity != 0u) || offset > media->size()) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const std::size_t start = static_cast<std::size_t>(offset);
  const std::size_t count =
      std::min(capacity, media->size() - start);
  if (count != 0u) {
    std::memcpy(buffer, media->data() + start, count);
  }
  *out_read = count;
  return H2_PAL_OK;
}

int should_stop(void *user) {
  return h2::desktop::poll_events(
      static_cast<h2::desktop::OwnedDisplay *>(user));
}

} // namespace

int h2_desktop_mp4_player_main(const h2::desktop::Layout &layout,
                               const char *media_resource) {
  h2::desktop::OwnedAudio audio;
  h2::desktop::OwnedDisplay display;
  if (h2::desktop::configure_layout(layout) != H2_PAL_OK ||
      h2::desktop::open_audio(false, &audio) != H2_AUDIO_OK ||
      h2::desktop::open_display(layout, &display) != H2_DISPLAY_OK) {
    std::fprintf(stderr, "desktop %s: platform configuration failed\n",
                 layout.app_name);
    return 1;
  }
  const std::string media_path = h2::desktop::resource_path(media_resource);
  std::ifstream stream(media_path, std::ios::binary);
  std::vector<std::uint8_t> media{
      std::istreambuf_iterator<char>(stream),
      std::istreambuf_iterator<char>()};
  if (!stream.eof() || media.empty()) {
    std::fprintf(stderr, "desktop %s: cannot load media %s\n",
                 layout.app_name, media_path.c_str());
    return 1;
  }

  h2_runtime_config_t config = h2::desktop::runtime_config(nullptr);
  config.display = display.display();
  config.audio = audio.api();
  config.audio_decoder = h2_ffmpeg_audio_decoder_api();
  h2_runtime_t *runtime = nullptr;
  const int runtime_result = h2_runtime_init(&config, &runtime);
  if (runtime_result != H2_PAL_OK || runtime == nullptr) {
    std::fprintf(stderr, "desktop %s: runtime init failed (%d)\n",
                 layout.app_name, runtime_result);
    return 1;
  }
  const h2_smoke_mp4_player_config_t player_config = {
      {&media, media.size(), read_at},
      nullptr,
      1000u,
      0u,
      1,
      H2_SMOKE_MP4_PLAYER_DISPLAY_EXACT,
      1,
      should_stop,
      &display,
      nullptr,
      nullptr,
  };
  const int app_result =
      h2_smoke_mp4_player_run(runtime, &player_config);
  if (app_result != H2_PAL_OK) {
    std::fprintf(stderr, "desktop %s: app failed (%d)\n",
                 layout.app_name, app_result);
  }
  h2_runtime_deinit(runtime);
  return app_result == H2_PAL_OK ? 0 : 1;
}
