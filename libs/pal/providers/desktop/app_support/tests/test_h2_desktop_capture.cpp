#include "h2_desktop_capture.h"
#include "h2_desktop_platform.h"
#include "h2_portaudio_internal.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>

namespace {
struct Observer {
  unsigned displays = 0u;
  unsigned microphones = 0u;
  uint16_t first_pixel = 0u;
  uint8_t brightness = 0u;
  uint64_t mic_timestamp = 0u;
  std::array<int16_t, 4> mic = {};
  std::mutex mutex;
  std::condition_variable changed;
  bool block_mic = false;
  bool entered = false;
  bool release = false;
};
void display_frame(void *user, const h2_sdl3_capture_frame_t *frame) {
  auto *observer = static_cast<Observer *>(user);
  assert(frame->width == 32u && frame->height == 24u);
  assert(frame->stride_bytes == 64u && frame->timestamp_us > 0u);
  ++observer->displays;
  observer->first_pixel = frame->pixels[0];
  observer->brightness = frame->brightness;
}
void mic_frame(void *user, const h2_portaudio_capture_frame_t *frame) {
  auto *observer = static_cast<Observer *>(user);
  assert(frame->sample_rate == 16000u && frame->channels == 1u);
  assert(frame->frames == observer->mic.size());
  std::copy_n(frame->samples, frame->frames, observer->mic.begin());
  observer->mic_timestamp = frame->timestamp_us;
  ++observer->microphones;
  std::unique_lock<std::mutex> lock(observer->mutex);
  observer->entered = true;
  observer->changed.notify_all();
  if (observer->block_mic) {
    observer->changed.wait(lock, [&] { return observer->release; });
  }
}
void speaker_frame(void *, const h2_portaudio_capture_frame_t *) {}
void present(h2_sdl3_t *provider) {
  assert(h2_pal_display_present(h2_sdl3_display(provider)) == H2_PAL_OK);
  h2_sdl3_event_t event = {};
  while (h2_sdl3_poll_event(provider, &event) == H2_PAL_OK) {
  }
}
} // namespace

int main() {
  assert(setenv("SDL_VIDEODRIVER", "dummy", 1) == 0);
  h2_desktop_capture_t *capture = nullptr;
  assert(h2_desktop_capture_create(nullptr, &capture) == H2_PAL_OK);
  assert(capture == nullptr);
  h2_desktop_capture_config_t config = {};
  assert(h2_desktop_capture_create(&config, &capture) == H2_PAL_OK);
  assert(capture == nullptr);
  config.hooks.on_display = display_frame;
  assert(h2_desktop_capture_create(&config, &capture) ==
         H2_PAL_ERR_INVALID_ARG);

  h2_sdl3_t *display = nullptr;
  const h2_sdl3_config_t display_config = {"Desktop capture test", 32, 24};
  assert(h2_sdl3_create(&display_config, &display) == H2_PAL_OK);
  assert(h2_pal_display_open(h2_sdl3_display(display)) == H2_PAL_OK);
  h2_portaudio_t *audio = nullptr;
  const h2_portaudio_config_t audio_config = {
      h2_desktop_platform_default_allocator(), h2_desktop_platform_queue_api(),
      h2_desktop_platform_sync_api(), 0};
  assert(h2_portaudio_create(&audio_config, &audio) == H2_PAL_OK);
  Observer observer;
  const std::array<int16_t, 4> samples = {-100, 0, 42, 32000};
  config = {
      display, audio, {&observer, display_frame, mic_frame, speaker_frame}};

  // Occupied audio rolls back this call's display hook, preserving its owner.
  const h2_portaudio_capture_hooks_t occupied = {nullptr, nullptr,
                                                 speaker_frame};
  assert(h2_portaudio_set_capture_hooks(audio, &occupied) == H2_PAL_OK);
  assert(h2_desktop_capture_create(&config, &capture) == H2_PAL_ERR_BUSY);
  assert(capture == nullptr);
  assert(h2_sdl3_set_frame_capture(display, display_frame, &observer) ==
         H2_PAL_OK);
  assert(h2_sdl3_set_frame_capture(display, nullptr, nullptr) == H2_PAL_OK);
  assert(h2_portaudio_set_capture_hooks(audio, &occupied) == H2_PAL_ERR_BUSY);
  assert(h2_portaudio_set_capture_hooks(audio, nullptr) == H2_PAL_OK);

  assert(h2_desktop_capture_create(&config, &capture) == H2_PAL_OK);
  const uint16_t pixel = 0xf800u;
  const h2_display_rect_t rect = {0, 0, 1, 1};
  assert(h2_pal_display_draw_bitmap(h2_sdl3_display(display), &rect, &pixel,
                                    sizeof(pixel),
                                    H2_DISPLAY_PIXEL_RGB565) == H2_PAL_OK);
  assert(h2_pal_display_set_brightness_percent(h2_sdl3_display(display), 50u) ==
         H2_PAL_OK);
  present(display);
  assert(observer.displays > 0u && observer.first_pixel == pixel);
  assert(observer.brightness == 128u);
  h2_portaudio_publish_mic_for_test(audio, samples.data(), samples.size(),
                                    1000u, 0);
  assert(observer.microphones == 0u); // synthetic fallback is not real capture
  h2_portaudio_publish_mic_for_test(audio, samples.data(), samples.size(),
                                    2000u, 1);
  assert(observer.microphones == 1u && observer.mic == samples);
  assert(observer.mic_timestamp == 2000u);

  // Destroy is a completion barrier for callbacks borrowing user state.
  observer.block_mic = true;
  observer.entered = false;
  std::thread producer([&] {
    h2_portaudio_publish_mic_for_test(audio, samples.data(), samples.size(),
                                      3000u, 1);
  });
  {
    std::unique_lock<std::mutex> lock(observer.mutex);
    observer.changed.wait(lock, [&] { return observer.entered; });
  }
  auto destroying = std::async(std::launch::async,
                               [&] { h2_desktop_capture_destroy(capture); });
  assert(destroying.wait_for(std::chrono::milliseconds(20)) ==
         std::future_status::timeout);
  {
    std::lock_guard<std::mutex> lock(observer.mutex);
    observer.release = true;
    observer.changed.notify_all();
  }
  producer.join();
  destroying.get();
  const unsigned displays = observer.displays;
  const unsigned microphones = observer.microphones;
  present(display);
  h2_portaudio_publish_mic_for_test(audio, samples.data(), samples.size(),
                                    4000u, 1);
  assert(observer.displays == displays && observer.microphones == microphones);
  h2_desktop_capture_destroy(nullptr);
  h2_portaudio_destroy(audio);
  h2_sdl3_destroy(display);
  return 0;
}
