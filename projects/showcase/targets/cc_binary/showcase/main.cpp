#include "h2_desktop_application_host.hpp"
#include "h2_showcase.h"
#include "layout_config.h"

#include "h2_ffmpeg.h"

#include "h2_atomic.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <memory>

namespace {

constexpr h2_runtime_component_mapping_entry_t kMappings[] = {
    {H2_SHOWCASE_COMPONENT_ACTION_BUTTON, 101u},
};

h2_pal_result_t read_pointer(void *user, h2_showcase_pointer_state_t *out) {
  if (user == nullptr || out == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_sdl3_pointer_state_t state = {};
  const int result = h2_desktop_application_host_pointer(
      static_cast<h2_application_host_t *>(user), &state);
  if (result != H2_PAL_OK) {
    return static_cast<h2_pal_result_t>(result);
  }
  *out = {state.x, state.y, state.pressed};
  return H2_PAL_OK;
}

struct AppContext {
  std::vector<std::uint8_t> font, audio;
};
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  auto *context = static_cast<AppContext *>(user);
  int opened = h2_pal_display_open(runtime->display);
  if (opened != H2_PAL_OK) return opened;
  const char *endpoint = std::getenv("H2_SHOWCASE_GIZCLAW_ENDPOINT");
  const char *private_key =
      std::getenv("H2_SHOWCASE_GIZCLAW_PRIVATE_KEY");
  const char *registration_token =
      std::getenv("H2_SHOWCASE_GIZCLAW_REGISTRATION_TOKEN");
  const h2_showcase_video_entry_t videos[] = {{
      "big-buck-bunny",
      "Big Buck Bunny",
      "/media/big_buck_bunny_showcase.mp4",
      context->audio.data(),
      context->audio.size(),
  }};
  const h2_showcase_character_entry_t characters[] = {
      {"tiga", "迪迦"},
      {"zero", "赛罗"},
  };
  const h2_showcase_config_t config = {
      videos,
      sizeof(videos) / sizeof(videos[0]),
      characters,
      sizeof(characters) / sizeof(characters[0]),
      context->font.data(),
      context->font.size(),
      endpoint,
      private_key,
      registration_token,
      endpoint == nullptr ? nullptr : "showcase",
      45000u,
      read_pointer,
      host,
      h2_application_host_should_stop,
      host,
  };
  return h2_showcase_run(runtime, &config);
}

} // namespace

int main() {
  constexpr h2::desktop::Layout layout = {
      h2_desktop_layout::app_name, h2_desktop_layout::title,
      h2_desktop_layout::width, h2_desktop_layout::height,
      h2_desktop_layout::mounts, h2_desktop_layout::mount_count,
      h2_desktop_layout::peripherals,
      h2_desktop_layout::peripheral_count,
      h2_desktop_layout::normalized_json,
  };
  std::fprintf(
      stderr,
      "Showcase controls: Space x10 opens console; hold Space 500ms "
      "starts conversation; mouse operates touch UI\n");
  auto context = std::make_unique<AppContext>();
  if (!h2::desktop::load_resource("projects/showcase/assets/fonts/NotoSansSC-Bold.ttf", &context->font) ||
      !h2::desktop::load_resource("projects/showcase/assets/media/big_buck_bunny_showcase_s16le_16k_mono.pcm", &context->audio))
    return 1;
  const h2_desktop_filesystem_mount_t mount = {"projects/showcase/assets/media", "/media"};
  auto platform = h2::desktop::application_host_config(layout,
      H2_DESKTOP_HOST_AUDIO | H2_DESKTOP_HOST_HTTP | H2_DESKTOP_HOST_WEBRTC | H2_DESKTOP_HOST_PREFERENCE);
  platform.mounts = &mount;
  platform.mount_count = 1u;
  platform.input = 1;
  platform.mappings = kMappings;
  platform.mapping_count = sizeof(kMappings) / sizeof(kMappings[0]);
  platform.entry_on_main = 0;
  h2_application_host_config_t app{};
  app.entry = run;
  app.user = context.get();
  h2_application_host_result_t result{};
  int rc = h2_desktop_application_host_run(&platform, &app, &result);
  if (result.retained) (void)context.release();
  return rc == H2_PAL_OK ? 0 : 1;
}
