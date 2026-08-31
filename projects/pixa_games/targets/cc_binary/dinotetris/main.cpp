#include "game_app.h"
#include "h2_dinotetris.h"
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

void on_event(void *, h2_dinotetris_event_t event) {
  const char *name = nullptr;
  switch (event) {
  case H2_DINOTETRIS_EVENT_ROTATE:
    name = "rotate";
    break;
  case H2_DINOTETRIS_EVENT_FAST_DROP:
    name = "fast_drop";
    break;
  case H2_DINOTETRIS_EVENT_PIECE_LOCKED:
    name = "piece_locked";
    break;
  case H2_DINOTETRIS_EVENT_GAME_OVER:
    name = "game_over";
    break;
  }
  if (name != nullptr) {
    std::fprintf(stderr, "dinotetris: %s\n", name);
  }
}

int create_game(const pixa_asset_t *, h2_game_audio_t *, void **out_game) {
  const h2_dinotetris_config_t config = {
      &game_text,
      h2_dinotetris_english_texts(),
      0x324d1eu,
      on_event,
      nullptr,
  };
  h2_dinotetris_t *game = nullptr;
  const int result = h2_dinotetris_create(&config, &game);
  *out_game = game;
  return result;
}

void destroy_game(void *game) {
  h2_dinotetris_destroy(static_cast<h2_dinotetris_t *>(game));
}

h2_game_scene_t *game_scene(void *game) {
  return h2_dinotetris_scene(static_cast<h2_dinotetris_t *>(game));
}

void handle_input(void *game, const h2_game_input_event_t *event) {
  h2_dinotetris_handle_input(static_cast<h2_dinotetris_t *>(game),
                             event);
}

bool handle_event(void *, h2_game_runtime_t *runtime,
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
  std::uint8_t button = 0u;
  if (event->component_id == kLeft) {
    button = H2_DINOTETRIS_BUTTON_LEFT;
  } else if (event->component_id == kRight) {
    button = H2_DINOTETRIS_BUTTON_RIGHT;
  } else if (event->component_id == kAction) {
    button = H2_DINOTETRIS_BUTTON_ACTION;
  } else {
    return true;
  }
  return h2::desktop::send_game_button(
      runtime, type, button, event->timestamp_ms, "dinotetris");
}

void print_result(void *game) {
  h2_dinotetris_result_t result = {};
  (void)h2_dinotetris_get_result(
      static_cast<h2_dinotetris_t *>(game), &result);
  std::fprintf(stderr,
               "dinotetris: score=%u lines=%u level=%u game_over=%u\n",
               result.score, result.lines_cleared, result.level,
               result.game_over);
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
      "dinotetris",
      "dinotetris: Left/Right move; tap Space rotates; hold Space fast-drops",
      nullptr,
      false,
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
