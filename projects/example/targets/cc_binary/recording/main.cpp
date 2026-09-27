#include "h2_desktop_app_support.h"
#include "h2_desktop_recording.h"
#include "h2_lvgl_platform.h"
#include "lvgl.h"

#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <thread>

namespace {
volatile std::sig_atomic_t stop_requested = 0;
void request_stop(int) { stop_requested = 1; }
} // namespace

// Real Desktop render + speaker path; no microphone, service or fake backend.
int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: recording-smoke <new-absolute-output.mp4>\n");
    return 2;
  }
  const auto *time = h2_desktop_platform_time_api();
  const h2_lvgl_platform_config_t platform = {
      h2_desktop_platform_default_allocator(),
      h2_desktop_platform_task_api(),
      h2_desktop_platform_sync_api(),
      h2_desktop_platform_queue_api(),
      time,
      0u,
      0u};
  if (h2_lvgl_platform_init(&platform) != H2_PAL_OK) {
    return 1;
  }
  lv_init();
  int result = H2_PAL_OK;
  {
    const h2::desktop::Layout layout = {"recording-smoke",
                                        "GizOS AV recording",
                                        320,
                                        240,
                                        nullptr,
                                        0u,
                                        nullptr,
                                        0u,
                                        "{}"};
    h2::desktop::OwnedDisplay display;
    h2::desktop::OwnedAudio audio;
    h2::desktop::OwnedLvgl lvgl;
    if (h2::desktop::open_display(layout, &display) != H2_PAL_OK ||
        h2::desktop::open_audio(true, &audio) != H2_PAL_OK ||
        h2::desktop::open_lvgl(&display, &lvgl) != H2_PAL_OK) {
      return 1;
    }
    h2_desktop_recording_t *recording = nullptr;
    result = h2_desktop_recording_start(display.handle, audio.handle, time,
                                        argv[1], &recording);
    if (result != H2_PAL_OK) {
      return 1;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x122236), 0);
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label,
                      "GizOS Desktop\nLVGL + real speaker\nStreaming MP4");
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_size(panel, 200, 90);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 60, 130);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x36deaa), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_50, 0);
    lv_obj_t *beat = lv_label_create(panel);
    lv_obj_center(beat);
    lv_label_set_text(beat, "440 Hz");
    h2_audio_info_t info = {};
    h2_pal_audio_track_t *track = nullptr;
    result = h2_pal_audio_get_info(audio.api(), &info);
    const h2_audio_track_config_t track_config = {
        "recording-tone", info.playback_format, 1000u, 4u, nullptr};
    if (result == H2_PAL_OK) {
      result = h2_pal_audio_start_speaker(audio.api());
    }
    if (result == H2_PAL_OK) {
      result = h2_pal_audio_create_track(audio.api(), &track_config, &track);
    }
    (void)std::signal(SIGINT, request_stop);
    (void)std::signal(SIGTERM, request_stop);
    uint64_t start = 0u;
    (void)h2_pal_time_get_monotonic_us(time, &start);
    uint64_t sample = 0u;
    std::array<int16_t, 320u> samples = {};
    for (unsigned tick = 0u; result == H2_PAL_OK && tick < 300u; ++tick) {
      if (stop_requested != 0 ||
          h2::desktop::poll_events(&display, &lvgl) != 0) {
        break;
      }
      const bool high = (tick / 50u) % 2u != 0u;
      const double frequency = high ? 880.0 : 440.0;
      lv_label_set_text(beat, high ? "880 Hz" : "440 Hz");
      lv_obj_set_style_bg_color(panel, lv_color_hex(high ? 0xf2af51 : 0x36deaa),
                                0);
      lv_obj_set_x(panel, 60 + static_cast<int>(15.0 * std::sin(tick * 0.06)));
      lv_tick_inc(20u);
      lv_timer_handler();
      lv_refr_now(static_cast<lv_display_t *>(lvgl.display()));
      (void)h2::desktop::poll_events(&display, &lvgl);
      for (int16_t &value : samples) {
        value = static_cast<int16_t>(
            5000.0 *
            std::sin(6.283185307179586 * frequency * sample++ / 16000.0));
      }
      h2_audio_frame_t frame = {samples.data(),
                                sizeof(samples),
                                sizeof(samples),
                                16000u,
                                320u,
                                1u,
                                H2_AUDIO_SAMPLE_S16LE};
      result = h2_pal_audio_track_write(track, &frame, 1000u);
      uint64_t now = 0u;
      (void)h2_pal_time_get_monotonic_us(time, &now);
      const uint64_t next = start + (tick + 1u) * 20000u;
      if (now < next) {
        std::this_thread::sleep_for(std::chrono::microseconds(next - now));
      }
    }
    if (track != nullptr) {
      const int drain = h2_pal_audio_track_drain(track, 3000u);
      const int closed = h2_pal_audio_track_close(track);
      if (result == H2_PAL_OK) {
        result = drain == H2_PAL_OK ? closed : drain;
      }
    }
    const int speaker_stop = h2_pal_audio_stop_speaker(audio.api());
    (void)h2::desktop::poll_events(&display, &lvgl);
    h2_ffmpeg_recording_stats_t stats = {};
    const int recorded = h2_desktop_recording_stop(recording, &stats);
    h2_desktop_recording_destroy(recording);
    std::printf("H2_RECORDING result=%d speaker_stop=%d duration_us=%llu "
                "video=%llu presented=%llu speaker_samples=%llu late=%llu "
                "buffer_bytes=%zu canceled=%d path=%s\n",
                recorded, speaker_stop,
                static_cast<unsigned long long>(stats.duration_us),
                static_cast<unsigned long long>(stats.video_frames),
                static_cast<unsigned long long>(stats.captured_video_frames),
                static_cast<unsigned long long>(stats.captured_audio_frames),
                static_cast<unsigned long long>(stats.late_audio_frames),
                stats.buffer_bytes, stop_requested != 0, argv[1]);
    if (recorded != H2_PAL_OK || speaker_stop != H2_PAL_OK ||
        stats.captured_video_frames == 0u ||
        stats.captured_audio_frames == 0u) {
      result = H2_PAL_ERR_IO;
    }
  }
  lv_deinit();
  h2_lvgl_platform_deinit();
  return result == H2_PAL_OK ? 0 : 1;
}
