#include "game_app.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace h2::desktop {
namespace {

const GameAdapter *active_adapter = nullptr;
void *active_game = nullptr;

h2_pal_result_t mapper_list(
    void *user, h2_runtime_component_t filter,
    h2_runtime_component_mapping_cb_t callback, void *callback_user) {
  const auto *adapter = static_cast<const GameAdapter *>(user);
  if (adapter == nullptr || callback == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (filter != H2_RUNTIME_COMPONENT_NONE &&
      filter != H2_RUNTIME_COMPONENT_BUTTON) {
    return H2_PAL_OK;
  }
  for (std::size_t index = 0; index < adapter->mapping_count; ++index) {
    if (callback(callback_user, &adapter->mappings[index]) != H2_PAL_OK) {
      return H2_PAL_ERR_IO;
    }
  }
  return H2_PAL_OK;
}

h2_pal_result_t mapper_get(
    void *user, h2_runtime_component_id_t component_id,
    h2_pal_periph_id_t *out_periph_id) {
  const auto *adapter = static_cast<const GameAdapter *>(user);
  if (adapter == nullptr || out_periph_id == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  for (std::size_t index = 0; index < adapter->mapping_count; ++index) {
    if (adapter->mappings[index].component_id == component_id) {
      *out_periph_id = adapter->mappings[index].periph_id;
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}

const h2_runtime_component_mapper_vtable_t mapper_vtable = {
    mapper_list,
    mapper_get,
};

void game_input(h2_game_scene_t *, const h2_game_input_event_t *event) {
  if (active_adapter != nullptr && active_game != nullptr &&
      active_adapter->handle_input != nullptr) {
    active_adapter->handle_input(active_game, event);
  }
}

} // namespace

bool send_game_button(h2_game_runtime_t *runtime,
                      h2_game_input_type_t type, std::uint8_t button,
                      std::uint64_t timestamp_ms, const char *game_name) {
  const h2_game_input_event_t input = {type, 0, 0, button};
  int result = h2_game_runtime_send_input(runtime, &input);
  if (result == H2_GAME_RUNTIME_ERR_OVERFLOW) {
    result = h2_game_runtime_tick(
        runtime, static_cast<std::uint32_t>(timestamp_ms));
    if (result == H2_GAME_RUNTIME_OK) {
      result = h2_game_runtime_send_input(runtime, &input);
    }
  }
  if (result != H2_GAME_RUNTIME_OK) {
    std::fprintf(stderr, "%s: failed to deliver input: %d\n", game_name,
                 result);
    return false;
  }
  return true;
}

int run_game(const Layout &layout, const GameAdapter &adapter) {
  OwnedAudio provider_audio;
  OwnedDisplay provider_display;
  if (configure_layout(layout) != H2_PAL_OK ||
      open_audio(false, &provider_audio) != H2_AUDIO_OK ||
      open_display(layout, &provider_display) != H2_DISPLAY_OK) {
    std::fprintf(stderr,
                 "desktop %s: display/input/audio provider initialization "
                 "failed\n",
                 layout.app_name);
    return 1;
  }
  OwnedFilesystem filesystem;
  if (open_filesystem(layout, &filesystem) != H2_PAL_FS_OK ||
      filesystem.handle == nullptr) {
    return 1;
  }
  const h2_pal_fs_api_t *fs = filesystem.api();
  const h2_runtime_component_mapper_t mapper = {
      const_cast<GameAdapter *>(&adapter),
      &mapper_vtable,
  };
  h2_runtime_config_t runtime_options = runtime_config(fs);
  runtime_options.display = provider_display.display();
  runtime_options.audio = provider_audio.api();
  runtime_options.component_mapper = &mapper;
  h2_runtime_t *runtime = nullptr;
  bool speaker_started = false;
  h2_game_audio_t *audio = nullptr;
  void *game = nullptr;
  h2_game_runtime_t *game_runtime = nullptr;
  int exit_code = 1;

  do {
    const int runtime_result =
        h2_runtime_init(&runtime_options, &runtime);
    if (runtime_result != H2_PAL_OK || runtime == nullptr) {
      std::fprintf(stderr, "%s: runtime initialization failed: %d\n",
                   adapter.name, runtime_result);
      break;
    }
    const h2_pal_result_t input_result =
        h2_runtime_input_start(runtime, nullptr);
    if (input_result != H2_PAL_OK) {
      std::fprintf(stderr, "%s: runtime input start failed: %d\n",
                   adapter.name, input_result);
      h2_runtime_deinit(runtime);
      runtime = nullptr;
      break;
    }
    std::vector<std::uint8_t> player_data;
    pixa_asset_t player = {};
    const pixa_asset_t *player_pointer = nullptr;
    if (adapter.player_path != nullptr) {
      if (!load_file(fs, adapter.player_path, &player_data) ||
          pixa_open_memory(player_data.data(), player_data.size(), &player) !=
              PIXA_OK) {
        break;
      }
      player_pointer = &player;
    }

    if (adapter.uses_audio) {
      if (h2_pal_audio_start_speaker(runtime->audio) != H2_AUDIO_OK) {
        std::fprintf(stderr, "%s: speaker initialization failed\n",
                     adapter.name);
        break;
      }
      speaker_started = true;
      const h2_game_audio_config_t audio_config = {
          runtime->audio,
          runtime->task,
          runtime->queue,
          runtime->mem,
          8u,
      };
      if (h2_game_audio_create(&audio_config, &audio) !=
              H2_GAME_AUDIO_OK ||
          audio == nullptr) {
        std::fprintf(stderr, "%s: real audio output initialization failed\n",
                     adapter.name);
        break;
      }
    }

    if (adapter.create(player_pointer, audio, &game) != 0 ||
        game == nullptr) {
      break;
    }
    active_adapter = &adapter;
    active_game = game;
    const h2_game_runtime_config_t game_config = {
        runtime->display,
        static_cast<std::uint16_t>(layout.width),
        static_cast<std::uint16_t>(layout.height),
        adapter.scene(game),
        game_input,
    };
    if (h2_game_runtime_create(&game_config, &game_runtime) !=
            H2_GAME_RUNTIME_OK ||
        game_runtime == nullptr) {
      break;
    }

    std::fprintf(stderr, "%s\n", adapter.help);
    std::array<std::uint8_t, H2_RUNTIME_EVENT_PAYLOAD_MAX> payload = {};
    constexpr auto frame_period =
        std::chrono::nanoseconds(1'000'000'000 / 60);
    auto next_frame = std::chrono::steady_clock::now();
    bool loop_failed = false;
    while (poll_events(&provider_display) == 0) {
      const auto before_frame = std::chrono::steady_clock::now();
      if (before_frame >= next_frame) {
        std::uint64_t now_ms = 0u;
        (void)h2_pal_time_get_monotonic_ms(runtime->time, &now_ms);
        const int tick_result = h2_game_runtime_tick(
            game_runtime, static_cast<std::uint32_t>(now_ms));
        if (tick_result != H2_GAME_RUNTIME_OK) {
          std::fprintf(stderr, "%s: game tick failed: %d\n", adapter.name,
                       tick_result);
          loop_failed = true;
          break;
        }
        if (adapter.after_tick != nullptr) {
          adapter.after_tick(game);
        }
        next_frame += frame_period;
        const auto after_frame = std::chrono::steady_clock::now();
        if (after_frame - next_frame > frame_period * 4) {
          next_frame = after_frame + frame_period;
        }
        continue;
      }
      h2_runtime_event_t event = {
          H2_RUNTIME_EVENT_NONE,
          H2_RUNTIME_COMPONENT_NONE,
          0u,
          0u,
          0u,
          payload.data(),
          payload.size(),
          0u,
      };
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              next_frame - before_frame + std::chrono::milliseconds(1));
      // Drain before waiting: a half-drained queue has no pending wake.
      while (h2_runtime_poll_event(runtime, &event) == H2_PAL_OK) {
        if (!adapter.handle_runtime_event(game, game_runtime, &event)) {
          loop_failed = true;
          break;
        }
      }
      if (loop_failed) {
        break;
      }
      const int result = h2_runtime_wait_notify(
          runtime, static_cast<std::uint32_t>(remaining.count()));
      if (result != H2_PAL_OK && result != H2_PAL_ERR_TIMEOUT) {
        std::fprintf(stderr, "%s: runtime wake wait failed: %d\n",
                     adapter.name, result);
        loop_failed = true;
        break;
      }
    }
    if (loop_failed) {
      exit_code = 1;
      break;
    }
    if (adapter.print_result != nullptr) {
      adapter.print_result(game);
    }
    exit_code = 0;
  } while (false);

  if (game_runtime != nullptr) {
    h2_game_runtime_destroy(game_runtime);
  }
  active_game = nullptr;
  active_adapter = nullptr;
  if (game != nullptr) {
    adapter.destroy(game);
  }
  if (audio != nullptr) {
    (void)h2_game_audio_destroy(audio);
  }
  if (speaker_started) {
    (void)h2_pal_audio_stop_speaker(runtime->audio);
  }
  if (runtime != nullptr) {
    h2_runtime_deinit(runtime);
  }
  return exit_code;
}

} // namespace h2::desktop
