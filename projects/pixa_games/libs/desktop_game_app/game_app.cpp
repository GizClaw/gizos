#include "game_app.h"
#include "h2_desktop_application_host.hpp"

#include <array>
#include <memory>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace h2::desktop {
namespace {

const GameAdapter *active_adapter = nullptr;
void *active_game = nullptr;

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

struct GameContext {
  const Layout *layout;
  const GameAdapter *adapter;
};
static h2_pal_result_t game_entry(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  auto *context = static_cast<GameContext *>(user);
  const auto &layout = *context->layout;
  const auto &adapter = *context->adapter;
  const auto *fs = runtime->fs;
  bool speaker_started = false;
  h2_game_audio_t *audio = nullptr;
  void *game = nullptr;
  h2_game_runtime_t *game_runtime = nullptr;
  int exit_code = 1;

  do {
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
    while (!h2_desktop_application_host_should_stop(host)) {
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
    int closed = h2_game_audio_destroy(audio);
    if (closed != H2_GAME_AUDIO_OK) {
      h2_application_host_retain(host, audio, H2_PAL_ERR_BUSY);
      return H2_PAL_ERR_BUSY;
    }
  }
  if (speaker_started) {
    (void)h2_pal_audio_stop_speaker(runtime->audio);
  }
  return exit_code == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}
int run_game(const Layout &layout, const GameAdapter &adapter) {
  auto context = std::make_unique<GameContext>();
  context->layout = &layout;
  context->adapter = &adapter;
  auto platform = application_host_config(layout, H2_DESKTOP_HOST_AUDIO);
  platform.input = 1;
  platform.mappings = adapter.mappings;
  platform.mapping_count = adapter.mapping_count;
  h2_application_host_config_t app{};
  app.entry = game_entry;
  app.user = context.get();
  h2_application_host_result_t result{};
  int rc = h2_desktop_application_host_run(&platform, &app, &result);
  if (result.retained) (void)context.release();
  return rc == H2_PAL_OK ? 0 : 1;
}

} // namespace h2::desktop
