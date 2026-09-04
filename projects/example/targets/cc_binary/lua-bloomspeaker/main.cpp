#include "h2_desktop_app_support.h"
#include "h2_bloomspeaker.h"
#include "layout_config.h"

#include <cstdio>

namespace {

constexpr h2_runtime_component_id_t kPowerComponentId =
    H2_BLOOMSPEAKER_COMPONENT_POWER;

constexpr h2::desktop::Layout kLayout = {
    h2_desktop_layout::app_name,        h2_desktop_layout::title,
    h2_desktop_layout::width,           h2_desktop_layout::height,
    h2_desktop_layout::mounts,          h2_desktop_layout::mount_count,
    h2_desktop_layout::peripherals,     h2_desktop_layout::peripheral_count,
    h2_desktop_layout::normalized_json,
};

h2_pal_result_t list_components(void *user, h2_runtime_component_t filter,
                                h2_runtime_component_mapping_cb_t callback,
                                void *callback_user) {
  (void)user;
  h2_runtime_component_mapping_entry_t entry{};
  if (callback == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (filter == H2_RUNTIME_COMPONENT_BUTTON) {
    entry = {kPowerComponentId, 2u};
    return callback(callback_user, &entry);
  }
  return H2_PAL_OK;
}

h2_pal_result_t get_periph_id(void *user,
                              h2_runtime_component_id_t component_id,
                              h2_pal_periph_id_t *out_periph_id) {
  (void)user;
  if (out_periph_id == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (component_id == kPowerComponentId) {
    *out_periph_id = 2u;
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

const h2_runtime_component_mapper_vtable_t kMapperVtable = {
    list_components,
    get_periph_id,
};

const h2_runtime_component_mapper_t kMapper = {
    nullptr,
    &kMapperVtable,
};

struct AppContext {
  h2::desktop::OwnedDisplay *display;
};

int should_stop(void *user) {
  auto *context = static_cast<AppContext *>(user);
  return context == nullptr || context->display == nullptr ||
         h2::desktop::poll_events(context->display) != 0;
}

} // namespace

int main() {
  h2::desktop::OwnedDisplay display;
  h2::desktop::OwnedAudio audio;
  if (h2::desktop::configure_layout(kLayout) != H2_PAL_OK ||
      h2::desktop::open_display(kLayout, &display) != H2_DISPLAY_OK ||
      h2::desktop::open_audio(false, &audio) != H2_AUDIO_OK) {
    std::fprintf(stderr,
                 "desktop %s: display or audio provider unavailable\n",
                 kLayout.app_name);
    return 1;
  }
  h2_pal_display_t *display_api = display.display();
  h2_runtime_config_t runtime_config = h2::desktop::runtime_config(nullptr);
  runtime_config.display = display_api;
  runtime_config.touch = display.touch();
  runtime_config.audio = audio.api();
  runtime_config.component_mapper = &kMapper;
  h2_runtime_t *runtime = nullptr;
  h2_pal_result_t result = h2_runtime_init(&runtime_config, &runtime);
  if (result != H2_PAL_OK) {
    std::fprintf(stderr, "desktop %s: Runtime init failed (%d)\n",
                 kLayout.app_name, result);
    return 1;
  }
  result = h2_runtime_input_start(runtime, nullptr);
  if (result != H2_PAL_OK) {
    std::fprintf(stderr, "desktop %s: Runtime input start failed (%d)\n",
                 kLayout.app_name, result);
    h2_runtime_deinit(runtime);
    return 1;
  }
  AppContext context = {&display};
  const h2_bloomspeaker_config_t config = {
      .back_component_id = kPowerComponentId,
      .power_component_id = H2_RUNTIME_COMPONENT_ID_NONE,
      .pairing_component_id = H2_RUNTIME_COMPONENT_ID_NONE,
      .should_stop = should_stop,
      .should_stop_user = &context,
      .on_ready = nullptr,
      .on_ready_user = nullptr,
      .pause_management_advertising = nullptr,
      .resume_management_advertising = nullptr,
      .management_advertising_user = nullptr,
  };
  result = h2_bloomspeaker_run(runtime, &config);
  (void)h2::desktop::poll_events(&display);
  h2_runtime_deinit(runtime);
  if (result != H2_PAL_OK) {
    std::fprintf(stderr, "desktop %s: App failed (%d)\n", kLayout.app_name,
                 result);
    return 1;
  }
  return 0;
}
