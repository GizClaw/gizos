// Desktop G99 Main. The layout selects the screen size and the keyboard
// mapping: Esc = AI (home), Space = BOOT (screen), arrows = up/down,
// Enter = OK, mouse = touch.
//
// Optional diagnostics, inert unless set:
//   H2_G99_SCRIPT       comma-separated "<delay_ms>:<command>" steps, where a
//                       command is home, screen, up, down, ok, snap:<name> or
//                       quit. Keys are pressed through the simulated buttons.
//   H2_G99_CAPTURE_DIR  directory receiving snap:<name> frames as <name>.bmp.

#include "g99_desktop.h"
#include "h2_desktop_capture.h"
#include "h2_g99.h"
#include "layout_config.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------- diagnostics: frame capture and scripted keys ----------

struct Capture {
  std::mutex mutex;
  std::vector<uint16_t> pixels;
  uint32_t width = 0u;
  uint32_t height = 0u;

  static void on_display(void* user, const h2_sdl3_capture_frame_t* frame) {
    auto* self = static_cast<Capture*>(user);
    if (self == nullptr || frame == nullptr || frame->pixels == nullptr) return;
    std::lock_guard<std::mutex> lock(self->mutex);
    self->width = frame->width;
    self->height = frame->height;
    self->pixels.resize(static_cast<size_t>(frame->width) * frame->height);
    const auto* bytes = reinterpret_cast<const uint8_t*>(frame->pixels);
    for (uint32_t y = 0; y < frame->height; ++y)
      std::memcpy(&self->pixels[static_cast<size_t>(y) * frame->width],
                  bytes + static_cast<size_t>(y) * frame->stride_bytes,
                  static_cast<size_t>(frame->width) * sizeof(uint16_t));
  }

  bool write_bmp(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex);
    if (pixels.empty()) return false;
    const uint32_t row = (width * 3u + 3u) & ~3u;
    const uint32_t size = 54u + row * height;
    std::vector<uint8_t> file(size, 0u);
    auto put32 = [&](size_t at, uint32_t v) {
      for (int i = 0; i < 4; ++i)
        file[at + i] = static_cast<uint8_t>(v >> (8 * i));
    };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, size);
    put32(10, 54u);
    put32(14, 40u);
    put32(18, width);
    put32(22, height);
    file[26] = 1u;
    file[28] = 24u;
    put32(34, row * height);
    for (uint32_t y = 0; y < height; ++y) {
      uint8_t* out = &file[54u + static_cast<size_t>(height - 1u - y) * row];
      for (uint32_t x = 0; x < width; ++x) {
        const uint16_t p = pixels[static_cast<size_t>(y) * width + x];
        const uint8_t r =
            static_cast<uint8_t>(((p >> 11) & 0x1Fu) * 255u / 31u);
        const uint8_t g = static_cast<uint8_t>(((p >> 5) & 0x3Fu) * 255u / 63u);
        const uint8_t b = static_cast<uint8_t>((p & 0x1Fu) * 255u / 31u);
        out[x * 3u] = b;
        out[x * 3u + 1u] = g;
        out[x * 3u + 2u] = r;
      }
    }
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(file.data()),
                 static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(stream);
  }
};

struct Step {
  uint32_t delay_ms;
  std::string command;
};

std::vector<Step> parse_script(const char* text) {
  std::vector<Step> steps;
  std::string all = text == nullptr ? "" : text;
  size_t start = 0;
  while (start < all.size()) {
    size_t end = all.find(',', start);
    if (end == std::string::npos) end = all.size();
    const std::string item = all.substr(start, end - start);
    const size_t colon = item.find(':');
    if (colon != std::string::npos)
      steps.push_back({static_cast<uint32_t>(std::strtoul(
                           item.substr(0, colon).c_str(), nullptr, 10)),
                       item.substr(colon + 1)});
    start = end + 1;
  }
  return steps;
}

bool key_of(const std::string& command, h2_desktop_key_t* out) {
  const struct {
    const char* name;
    h2_desktop_key_t key;
  } kKeys[] = {{"home", H2_DESKTOP_KEY_ESCAPE},
               {"screen", H2_DESKTOP_KEY_SPACE},
               {"up", H2_DESKTOP_KEY_UP},
               {"down", H2_DESKTOP_KEY_DOWN},
               {"ok", H2_DESKTOP_KEY_ENTER}};
  for (const auto& entry : kKeys) {
    if (command == entry.name) {
      *out = entry.key;
      return true;
    }
  }
  return false;
}

// ---------- Main worker ----------

struct AppContext {
  h2_runtime_t* runtime;
  const char* board;
  std::atomic<bool>* stop;
  h2_pal_result_t result = H2_PAL_OK;
};

int should_stop(void* user) {
  return static_cast<std::atomic<bool>*>(user)->load() ? 1 : 0;
}

void observe(void*, const h2_g99_observation_t* o) {
  std::fprintf(stderr,
               "G99_OBS seq=%u lifecycle=%d route=%d target=%d transition=%d "
               "main=%u sub=%u editing=%d edit=%u ready=%d screen=%d "
               "volume=%u brightness=%u timeout=%u size=%dx%d\n",
               o->sequence, o->lifecycle, o->route, o->target_route,
               o->transition, o->main_selection, o->sub_selection,
               o->editing ? 1 : 0, o->edit_index, o->input_ready ? 1 : 0,
               o->screen_on ? 1 : 0, o->volume_percent, o->brightness_percent,
               o->screen_timeout_index, o->width, o->height);
}

void run_main(AppContext* context) {
  h2_g99_config_t config = {};
  config.main_version = "desktop-dev";
  config.board_name = context->board;
  config.should_stop = should_stop;
  config.should_stop_user = context->stop;
  config.observe = observe;
  context->result = h2_g99_run(context->runtime, &config);
  std::fprintf(stderr, "g99 desktop: main exited rc=%d\n", context->result);
  context->stop->store(true);
}

}  // namespace

int main() {
  constexpr h2::desktop::Layout layout = {
      h2_desktop_layout::app_name,        h2_desktop_layout::title,
      h2_desktop_layout::width,           h2_desktop_layout::height,
      h2_desktop_layout::mounts,          h2_desktop_layout::mount_count,
      h2_desktop_layout::peripherals,     h2_desktop_layout::peripheral_count,
      h2_desktop_layout::normalized_json,
  };
  std::fprintf(stderr,
               "GizPocket controls: Esc=AI (home), Space=BOOT (screen), "
               "Up/Down=rotate, Enter=OK, mouse=touch\n");
  h2::desktop::g99::Platform platform;
  if (platform.open(layout) != H2_PAL_OK) {
    std::fprintf(stderr, "g99 desktop: platform configuration failed\n");
    return 1;
  }
  h2::desktop::OwnedDisplay& display_provider = *platform.display();
  h2_runtime_t* runtime = platform.runtime();
  Capture capture;
  h2_desktop_capture_t* capture_hook = nullptr;
  const char* capture_dir = std::getenv("H2_G99_CAPTURE_DIR");
  if (capture_dir != nullptr && capture_dir[0] != '\0') {
    const h2_desktop_capture_config_t capture_config = {
        display_provider.handle,
        nullptr,
        {&capture, Capture::on_display, nullptr, nullptr}};
    if (h2_desktop_capture_create(&capture_config, &capture_hook) !=
        H2_PAL_OK) {
      std::fprintf(stderr, "g99 desktop: capture unavailable\n");
      capture_hook = nullptr;
    }
  }
  std::atomic<bool> stop(false);
  AppContext context = {runtime, layout.app_name, &stop};
  std::thread worker;
  try {
    worker = std::thread(run_main, &context);
  } catch (...) {
    if (capture_hook != nullptr) h2_desktop_capture_destroy(capture_hook);
    return 1;
  }
  const std::vector<Step> script = parse_script(std::getenv("H2_G99_SCRIPT"));
  size_t next = 0;
  auto due =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(script.empty() ? 0u : script[0].delay_ms);
  h2_desktop_key_t held = H2_DESKTOP_KEY_SPACE;
  bool holding = false;
  auto release_at = std::chrono::steady_clock::now();
  while (!stop.load() && h2::desktop::poll_events(&display_provider) == 0) {
    const auto now = std::chrono::steady_clock::now();
    if (holding && now >= release_at) {
      (void)h2_desktop_platform_handle_key(held, 0, 0);
      holding = false;
    }
    if (!holding && next < script.size() && now >= due) {
      const std::string& command = script[next].command;
      if (command.rfind("snap:", 0) == 0 && capture_dir != nullptr) {
        const std::string path =
            std::string(capture_dir) + "/" + command.substr(5) + ".bmp";
        std::fprintf(stderr, "g99 desktop: snap %s %s\n", path.c_str(),
                     capture.write_bmp(path) ? "ok" : "failed");
      } else if (command == "quit") {
        stop.store(true);
      } else if (key_of(command, &held)) {
        (void)h2_desktop_platform_handle_key(held, 1, 0);
        holding = true;
        release_at = now + std::chrono::milliseconds(150);
      }
      ++next;
      if (next < script.size())
        due = now + std::chrono::milliseconds(script[next].delay_ms);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  stop.store(true);
  worker.join();
  if (capture_hook != nullptr) h2_desktop_capture_destroy(capture_hook);
  const int cleanup = platform.close();
  return context.result == H2_PAL_OK && cleanup == H2_PAL_OK ? 0 : 1;
}
