#include "game_app.h"
#include "h2_dinodive.h"
#include "layout_config.h"

#include <cstdio>

namespace {

constexpr std::uint32_t kLeft = 1u;
constexpr std::uint32_t kRight = 2u;
constexpr std::uint32_t kAction = 3u;
constexpr std::uint32_t kReset = 4u;
constexpr h2_runtime_component_mapping_entry_t kMappings[] = {
    {kLeft, 1u},
    {kRight, 2u},
    {kAction, 3u},
    {kReset, 4u},
};
h2_game_text_api_t game_text = h2_game_text_builtin_5x7();

void on_event(void *, h2_dinodive_event_t event) {
  if (event == H2_DINODIVE_EVENT_FALL) {
    std::fprintf(stderr, "dinodive: fall\n");
  } else if (event == H2_DINODIVE_EVENT_GAME_OVER) {
    std::fprintf(stderr, "dinodive: game_over\n");
  }
}

int create_game(const pixa_asset_t *player, h2_game_audio_t *audio,
                void **out_game) {
  h2_dinodive_config_t config = {
      player,
      audio,
      &game_text,
      h2_dinodive_english_texts(),
      0x324d1eu,
      on_event,
      nullptr,
  };
  h2_dinodive_t *game = nullptr;
  const int result = h2_dinodive_create(&config, &game);
  *out_game = game;
  return result;
}

void destroy_game(void *game) {
  h2_dinodive_destroy(static_cast<h2_dinodive_t *>(game));
}

h2_game_scene_t *game_scene(void *game) {
  return h2_dinodive_scene(static_cast<h2_dinodive_t *>(game));
}

void handle_input(void *game, const h2_game_input_event_t *event) {
  h2_dinodive_handle_input(static_cast<h2_dinodive_t *>(game), event);
}

bool handle_event(void *game, h2_game_runtime_t *runtime,
                  const h2_runtime_event_t *event) {
  if (event->component != H2_RUNTIME_COMPONENT_BUTTON) {
    return true;
  }
  if (event->kind != H2_RUNTIME_COMPONENT_EVENT_BUTTON_ACTION ||
      event->payload_size < sizeof(h2_runtime_button_action_event_t)) {
    return true;
  }
  const auto *action =
      static_cast<const h2_runtime_button_action_event_t *>(event->payload);
  h2_game_input_type_t type;
  if (h2_runtime_button_action_is_pressed(action)) {
    type = H2_GAME_INPUT_BUTTON_DOWN;
  } else if (h2_runtime_button_action_is_released(action)) {
    type = H2_GAME_INPUT_BUTTON_UP;
  } else {
    return true;
  }
  if (event->component_id == kReset) {
    if (type == H2_GAME_INPUT_BUTTON_DOWN) {
      h2_dinodive_reset(static_cast<h2_dinodive_t *>(game));
    }
    return true;
  }
  std::uint8_t button = 0u;
  if (event->component_id == kLeft) {
    button = H2_DINODIVE_BUTTON_LEFT;
  } else if (event->component_id == kRight) {
    button = H2_DINODIVE_BUTTON_RIGHT;
  } else if (event->component_id == kAction) {
    button = H2_DINODIVE_BUTTON_ACTION;
  } else {
    return true;
  }
  return h2::desktop::send_game_button(
      runtime, type, button, event->timestamp_ms, "dinodive");
}

void print_result(void *game) {
  h2_dinodive_result_t result = {};
  (void)h2_dinodive_get_result(
      static_cast<h2_dinodive_t *>(game), &result);
  std::fprintf(stderr, "dinodive: final floor_count=%d\n",
               result.floor_count);
}

} // namespace

int main() {
  constexpr h2::desktop::Layout layout = {
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
  constexpr h2::desktop::GameAdapter adapter = {
      "dinodive",
      "dinodive: Space starts; Left/Right change direction; R resets",
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
