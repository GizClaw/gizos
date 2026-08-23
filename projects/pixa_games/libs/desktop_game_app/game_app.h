#ifndef H2_DESKTOP_GAME_APP_H
#define H2_DESKTOP_GAME_APP_H

#include "h2_desktop_app_support.h"
#include "h2_game_audio.h"
#include "h2_game_runtime.h"
#include "pixa.h"

#include <cstddef>

namespace h2::desktop {

struct GameAdapter {
  const char *name;
  const char *help;
  const char *player_path;
  bool uses_audio;
  const h2_runtime_component_mapping_entry_t *mappings;
  std::size_t mapping_count;
  int (*create)(const pixa_asset_t *player, h2_game_audio_t *audio,
                void **out_game);
  void (*destroy)(void *game);
  h2_game_scene_t *(*scene)(void *game);
  void (*handle_input)(void *game, const h2_game_input_event_t *event);
  bool (*handle_runtime_event)(void *game, h2_game_runtime_t *runtime,
                               const h2_runtime_event_t *event);
  void (*print_result)(void *game);
  void (*after_tick)(void *game);
};

int run_game(const Layout &layout, const GameAdapter &adapter);
bool send_game_button(h2_game_runtime_t *runtime,
                      h2_game_input_type_t type, std::uint8_t button,
                      std::uint64_t timestamp_ms, const char *game_name);

} // namespace h2::desktop

#endif // H2_DESKTOP_GAME_APP_H
