#include "player.h"

#include "h2_ffmpeg.h"

#include "h2_desktop_application_host.hpp"
#include "h2_smoke_mp4_player.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>

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

} // namespace

static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  auto *media = static_cast<std::vector<std::uint8_t> *>(user);
  const h2_smoke_mp4_player_config_t player_config = {
      {media, media->size(), read_at},
      nullptr,
      1000u,
      0u,
      1,
      H2_SMOKE_MP4_PLAYER_DISPLAY_EXACT,
      1,
      h2_desktop_application_host_should_stop,
      host,
      nullptr,
      nullptr,
      false,
  };
  const int app_result =
      h2_smoke_mp4_player_run(runtime, &player_config);
  return app_result;
}
int h2_desktop_mp4_player_main(const h2::desktop::Layout &layout, const char *media_resource) {
  const std::string media_path = h2::desktop::resource_path(media_resource);
  auto media = std::make_unique<std::vector<std::uint8_t>>();
  if (!h2::desktop::load_resource(media_resource, media.get()) || media->empty()) {
    std::fprintf(stderr, "desktop %s: cannot load media %s\n",
                 layout.app_name, media_path.c_str());
    return 1;
  }

  auto platform = h2::desktop::application_host_config(layout, H2_DESKTOP_HOST_AUDIO);
  h2_application_host_config_t app{};
  app.entry = run;
  app.user = media.get();
  h2_application_host_result_t result{};
  int rc = h2_desktop_application_host_run(&platform, &app, &result);
  if (result.retained) (void)media.release();
  return rc == H2_PAL_OK ? 0 : 1;
}
