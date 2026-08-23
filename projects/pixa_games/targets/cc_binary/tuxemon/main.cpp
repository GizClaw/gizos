#include "game_app.h"
#include "h2_tuxemon.h"
#include "layout_config.h"

#include <cstdio>

namespace {

constexpr std::uint32_t kUp = 1u;
constexpr std::uint32_t kDown = 2u;
constexpr std::uint32_t kLeft = 3u;
constexpr std::uint32_t kRight = 4u;
constexpr h2_runtime_component_mapping_entry_t kMappings[] = {
    {kUp, 1u}, {kDown, 2u}, {kLeft, 3u}, {kRight, 4u},
};
std::uint8_t last_room = 0u;

int create_game(const pixa_asset_t *player, h2_game_audio_t *,
                void **out_game) {
  const h2_tuxemon_config_t config = {player};
  h2_tuxemon_t *game = nullptr;
  const int result = h2_tuxemon_create(&config, &game);
  *out_game = game;
  if (result == H2_TUXEMON_OK && game != nullptr) {
    h2_tuxemon_state_t state = {};
    if (h2_tuxemon_get_state(game, &state) == H2_TUXEMON_OK) {
      last_room = state.room;
    }
  }
  return result;
}

void destroy_game(void *game) {
  h2_tuxemon_destroy(static_cast<h2_tuxemon_t *>(game));
}

h2_game_scene_t *game_scene(void *game) {
  return h2_tuxemon_scene(static_cast<h2_tuxemon_t *>(game));
}

void handle_input(void *game, const h2_game_input_event_t *event) {
  h2_tuxemon_handle_input(static_cast<h2_tuxemon_t *>(game), event);
}

bool handle_event(void *, h2_game_runtime_t *runtime,
                  const h2_runtime_event_t *event) {
  if (event->component != H2_RUNTIME_COMPONENT_BUTTON) {
    return true;
  }
  h2_game_input_type_t type;
  if (event->kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_DOWN) {
    type = H2_GAME_INPUT_BUTTON_DOWN;
  } else if (event->kind == H2_RUNTIME_COMPONENT_EVENT_BUTTON_UP) {
    type = H2_GAME_INPUT_BUTTON_UP;
  } else {
    return true;
  }
  std::uint8_t button = 0u;
  if (event->component_id == kUp) {
    button = H2_TUXEMON_BUTTON_UP;
  } else if (event->component_id == kDown) {
    button = H2_TUXEMON_BUTTON_DOWN;
  } else if (event->component_id == kLeft) {
    button = H2_TUXEMON_BUTTON_LEFT;
  } else if (event->component_id == kRight) {
    button = H2_TUXEMON_BUTTON_RIGHT;
  } else {
    return true;
  }
  return h2::desktop::send_game_button(
      runtime, type, button, event->timestamp_ms, "tuxemon");
}

void after_tick(void *game) {
  h2_tuxemon_state_t state = {};
  if (h2_tuxemon_get_state(static_cast<h2_tuxemon_t *>(game), &state) ==
          H2_TUXEMON_OK &&
      state.room != last_room) {
    last_room = state.room;
    std::fprintf(stderr, "tuxemon: entered room %u\n", last_room);
  }
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
      "tuxemon",
      "tuxemon: arrow keys move; walk onto a doorway to enter or leave",
      "/pixa/player/player.pixa",
      false,
      kMappings,
      sizeof(kMappings) / sizeof(kMappings[0]),
      create_game,
      destroy_game,
      game_scene,
      handle_input,
      handle_event,
      nullptr,
      after_tick,
  };
  return h2::desktop::run_game(layout, adapter);
}
