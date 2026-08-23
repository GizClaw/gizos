#include "h2_desktop_app_support.h"
#include "h2_corehttp.h"
#include "h2_showcase.h"
#include "layout_config.h"

#include "h2_ffmpeg.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

constexpr h2_runtime_component_mapping_entry_t kMappings[] = {
    {H2_SHOWCASE_COMPONENT_ACTION_BUTTON, 101u},
};

h2_pal_result_t mapper_list(
    void *, h2_runtime_component_t filter,
    h2_runtime_component_mapping_cb_t callback, void *user) {
  if (callback == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (filter != H2_RUNTIME_COMPONENT_NONE &&
      filter != H2_RUNTIME_COMPONENT_BUTTON) {
    return H2_PAL_OK;
  }
  for (const auto &entry : kMappings) {
    if (callback(user, &entry) != H2_PAL_OK) {
      return H2_PAL_ERR_IO;
    }
  }
  return H2_PAL_OK;
}

h2_pal_result_t mapper_get(void *, h2_runtime_component_id_t component_id,
                           h2_pal_periph_id_t *out_periph_id) {
  if (out_periph_id == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  for (const auto &entry : kMappings) {
    if (entry.component_id == component_id) {
      *out_periph_id = entry.periph_id;
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}

const h2_runtime_component_mapper_vtable_t kMapperVtable = {
    mapper_list, mapper_get};
const h2_runtime_component_mapper_t kMapper = {nullptr, &kMapperVtable};

h2_pal_result_t read_pointer(void *user, h2_showcase_pointer_state_t *out) {
  if (user == nullptr || out == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_sdl3_pointer_state_t state = {};
  const int result = h2::desktop::read_pointer(
      static_cast<h2::desktop::OwnedDisplay *>(user), &state);
  if (result != H2_PAL_OK) {
    return static_cast<h2_pal_result_t>(result);
  }
  *out = {state.x, state.y, state.pressed};
  return H2_PAL_OK;
}

int should_stop(void *user) {
  const auto *stop = static_cast<std::atomic<bool> *>(user);
  return stop != nullptr &&
         stop->load(std::memory_order_acquire);
}

struct AppContext {
  h2_runtime_t *runtime;
  const std::vector<std::uint8_t> *font;
  const std::vector<std::uint8_t> *audio;
  h2::desktop::OwnedDisplay *display;
  std::atomic<bool> *stop;
  h2_pal_result_t result = H2_PAL_OK;
};

void app_main(AppContext *context) {
  const char *endpoint = std::getenv("H2_SHOWCASE_GIZCLAW_ENDPOINT");
  const char *private_key =
      std::getenv("H2_SHOWCASE_GIZCLAW_PRIVATE_KEY");
  const char *registration_token =
      std::getenv("H2_SHOWCASE_GIZCLAW_REGISTRATION_TOKEN");
  const h2_showcase_video_entry_t videos[] = {{
      "big-buck-bunny",
      "Big Buck Bunny",
      "/media/big_buck_bunny_showcase.mp4",
      context->audio->data(),
      context->audio->size(),
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
      context->font->data(),
      context->font->size(),
      endpoint,
      private_key,
      registration_token,
      endpoint == nullptr ? nullptr : "showcase",
      45000u,
      read_pointer,
      context->display,
      should_stop,
      context->stop,
  };
  context->result = h2_showcase_run(context->runtime, &config);
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
  h2::desktop::OwnedAudio audio;
  h2::desktop::OwnedDisplay display_provider;
  h2::desktop::OwnedNetworkServices network;
  if (h2::desktop::configure_layout(layout) != H2_PAL_OK ||
      h2::desktop::open_audio(false, &audio) != H2_AUDIO_OK ||
      h2::desktop::open_display(layout, &display_provider) != H2_DISPLAY_OK ||
      h2::desktop::open_network_services(false, true, &network) != H2_PAL_OK) {
    std::fprintf(stderr,
                 "showcase desktop: platform configuration failed\n");
    return 1;
  }
  h2::desktop::OwnedPreference preference;
  if (h2::desktop::open_preference(&preference) != H2_PAL_OK) {
    std::fprintf(stderr, "showcase desktop: preference initialization failed\n");
    return 1;
  }
  std::vector<std::uint8_t> font;
  std::vector<std::uint8_t> audio_pcm;
  if (!h2::desktop::load_resource(
          "projects/showcase/assets/fonts/NotoSansSC-Bold.ttf", &font) ||
      !h2::desktop::load_resource(
          "projects/showcase/assets/media/"
          "big_buck_bunny_showcase_s16le_16k_mono.pcm",
          &audio_pcm)) {
    std::fprintf(stderr, "showcase desktop: packaged assets missing\n");
    return 1;
  }
  const h2::desktop::FilesystemMount mount = {
      "projects/showcase/assets/media", "/media"};
  h2::desktop::Layout filesystem_layout = layout;
  filesystem_layout.mounts = &mount;
  filesystem_layout.mount_count = 1u;
  h2::desktop::OwnedFilesystem filesystem;
  if (h2::desktop::open_filesystem(filesystem_layout, &filesystem) !=
          H2_PAL_FS_OK ||
      filesystem.handle == nullptr) {
    std::fprintf(stderr,
                 "showcase desktop: filesystem initialization failed\n");
    return 1;
  }
  const h2_corehttp_config_t http_config = {
      .allocator = h2_desktop_platform_default_allocator(),
      .net = h2::desktop::host_net_api(),
      .time = h2_desktop_platform_time_api(),
      .log = h2_desktop_platform_log_api(),
      .tls_verify = H2_PAL_NET_TLS_VERIFY_DEFAULT,
      .root_ca_pem = nullptr,
      .root_ca_pem_len = 0u,
      .max_header_bytes = 0u,
      .max_redirects = 0u,
      .default_timeout_ms = 0u,
      .io_slice_ms = 0u,
  };
  h2_corehttp_t *http_provider = nullptr;
  h2_pal_http_api_t http_api = {};
  if (h2_corehttp_create(&http_config, &http_provider, &http_api) !=
      H2_PAL_OK) {
    return 1;
  }
  h2_runtime_config_t runtime_options = h2::desktop::runtime_config(
      filesystem.api());
  runtime_options.pref = preference.api();
  runtime_options.crypto = network.crypto();
  runtime_options.http = &http_api;
  runtime_options.net = h2::desktop::host_net_api();
  runtime_options.webrtc = network.webrtc();
  runtime_options.display = display_provider.display();
  runtime_options.audio = audio.api();
  runtime_options.audio_decoder =
      h2_ffmpeg_audio_decoder_api();
  runtime_options.component_mapper = &kMapper;
  h2_runtime_t *runtime = nullptr;
  if (h2_runtime_init(&runtime_options, &runtime) != H2_PAL_OK ||
      runtime == nullptr) {
    h2_corehttp_destroy(http_provider);
    return 1;
  }
  if (h2_runtime_input_start(runtime, nullptr) != H2_PAL_OK) {
    h2_runtime_deinit(runtime);
    h2_corehttp_destroy(http_provider);
    return 1;
  }
  h2_pal_display_t *display = display_provider.display();
  if (h2_pal_display_open(display) != H2_DISPLAY_OK) {
    h2_runtime_deinit(runtime);
    h2_corehttp_destroy(http_provider);
    return 1;
  }
  std::atomic<bool> stop = false;
  AppContext context = {runtime, &font, &audio_pcm, &display_provider, &stop,
                        H2_PAL_OK};
  std::thread worker;
  try {
    worker = std::thread(app_main, &context);
  } catch (...) {
    (void)h2_pal_display_close(display);
    h2_runtime_deinit(runtime);
    h2_corehttp_destroy(http_provider);
    return 1;
  }
  while (h2::desktop::poll_events(&display_provider) == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  stop.store(true, std::memory_order_release);
  worker.join();
  (void)h2_pal_display_close(display);
  h2_runtime_deinit(runtime);
  h2_corehttp_destroy(http_provider);
  if (context.result != H2_PAL_OK) {
    std::fprintf(stderr, "showcase desktop: app exited rc=%d\n",
                 context.result);
    return 1;
  }
  return 0;
}
