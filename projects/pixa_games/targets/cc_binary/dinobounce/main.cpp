#include "game_app.h"
#include "h2_dinobounce.h"
#include "layout_config.h"

#include <cstdio>

namespace {

constexpr std::uint32_t kLeft = 1u;
constexpr std::uint32_t kRight = 2u;
constexpr std::uint32_t kAction = 3u;
constexpr h2_runtime_component_mapping_entry_t kMappings[] = {
    {kLeft, 1u}, {kRight, 2u}, {kAction, 3u},
};
h2_game_text_api_t game_text = h2_game_text_builtin_5x7();

void on_event(void *, h2_dinobounce_event_t event) {
  if (event == H2_DINOBOUNCE_EVENT_BOUNCE) {
    std::fprintf(stderr, "dinobounce: bounce\n");
  } else if (event == H2_DINOBOUNCE_EVENT_GAME_OVER) {
    std::fprintf(stderr, "dinobounce: game_over\n");
  }
}

int create_game(const pixa_asset_t *player, h2_game_audio_t *audio,
                void **out_game) {
  const h2_dinobounce_config_t config = {
      player, audio, &game_text, h2_dinobounce_english_texts(),
      0x324d1eu, on_event, nullptr,
  };
  h2_dinobounce_t *game = nullptr;
  const int result = h2_dinobounce_create(&config, &game);
  *out_game = game;
  return result;
}

void destroy_game(void *game) {
  h2_dinobounce_destroy(static_cast<h2_dinobounce_t *>(game));
}

h2_game_scene_t *game_scene(void *game) {
  return h2_dinobounce_scene(static_cast<h2_dinobounce_t *>(game));
}

void handle_input(void *game, const h2_game_input_event_t *event) {
  h2_dinobounce_handle_input(static_cast<h2_dinobounce_t *>(game),
                             event);
}

bool handle_event(void *, h2_game_runtime_t *runtime,
                  const h2_runtime_event_t *event) {
  if (event->component != H2_RUNTIME_COMPONENT_BUTTON ||
      event->kind != H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION) {
    return true;
  }
  std::uint8_t button = 0u;
  if (event->component_id == kLeft) {
    button = H2_DINOBOUNCE_BUTTON_LEFT;
  } else if (event->component_id == kRight) {
    button = H2_DINOBOUNCE_BUTTON_RIGHT;
  } else if (event->component_id == kAction) {
    button = H2_DINOBOUNCE_BUTTON_ACTION;
  } else {
    return true;
  }
  return h2::desktop::send_game_button(
      runtime, H2_GAME_INPUT_BUTTON_CLICK, button, event->timestamp_ms,
      "dinobounce");
}

void print_result(void *game) {
  h2_dinobounce_result_t result = {};
  (void)h2_dinobounce_get_result(
      static_cast<h2_dinobounce_t *>(game), &result);
  std::fprintf(stderr,
               "dinobounce: survival_ms=%u game_over=%u\n",
               result.survival_ms, result.game_over);
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
  constexpr h2::desktop::GameAdapter adapter = {
      "dinobounce",
      "dinobounce: Left/Right move; Space launches and retries",
      "/pixa/player/player.pixa",
      true,
      kMappings,
      sizeof(kMappings) / sizeof(kMappings[0]),
      create_game,
      destroy_game,
      game_scene,
      handle_input,
      handle_event,
      print_result,
      nullptr,
  };
  return h2::desktop::run_game(layout, adapter);
}
