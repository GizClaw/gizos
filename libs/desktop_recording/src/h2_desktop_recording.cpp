#include "h2_desktop_recording_internal.h"

#include <new>

struct h2_desktop_recording {
  h2_sdl3_t *display = nullptr;
  h2_portaudio_t *audio = nullptr;
  h2_desktop_recording_encoder_t *encoder = nullptr;
  bool display_attached = false;
  bool audio_attached = false;
};

extern "C" {
h2_pal_result_t
h2_desktop_recording_start(h2_sdl3_t *display, h2_portaudio_t *audio,
                           const char *path,
                           h2_desktop_recording_t **out_recording) {
  if (out_recording == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_recording = nullptr;
  if (display == nullptr || audio == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_display_info_t info = {};
  h2_pal_result_t result = static_cast<h2_pal_result_t>(
      h2_pal_display_get_info(h2_sdl3_display(display), &info));
  if (result != H2_PAL_OK) {
    return result;
  }
  auto *state = new (std::nothrow) h2_desktop_recording_t();
  if (state == nullptr) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  state->display = display;
  state->audio = audio;
  const h2_desktop_recording_encoder_config_t config = {
      path, static_cast<uint32_t>(info.width),
      static_cast<uint32_t>(info.height), h2_desktop_recording_now_us()};
  result = h2_desktop_recording_encoder_create(&config, &state->encoder);
  if (result == H2_PAL_OK) {
    result = h2_sdl3_set_frame_capture(
        display, h2_desktop_recording_encoder_video, state->encoder);
    state->display_attached = result == H2_PAL_OK;
    if (result == H2_PAL_OK) {
      result = h2_portaudio_set_output_capture(
          audio, h2_desktop_recording_encoder_audio, state->encoder);
      state->audio_attached = result == H2_PAL_OK;
    }
  }
  if (result != H2_PAL_OK) {
    h2_desktop_recording_destroy(state);
    return result;
  }
  *out_recording = state;
  return H2_PAL_OK;
}

h2_pal_result_t
h2_desktop_recording_stop(h2_desktop_recording_t *state,
                          h2_desktop_recording_stats_t *out_stats) {
  if (out_stats != nullptr) {
    *out_stats = {};
  }
  if (state == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (state->display_attached) {
    (void)h2_sdl3_set_frame_capture(state->display, nullptr, nullptr);
    state->display_attached = false;
  }
  if (state->audio_attached) {
    (void)h2_portaudio_set_output_capture(state->audio, nullptr, nullptr);
    state->audio_attached = false;
  }
  return h2_desktop_recording_encoder_finish(
      state->encoder, h2_desktop_recording_now_us(), out_stats);
}

void h2_desktop_recording_destroy(h2_desktop_recording_t *state) {
  if (state != nullptr) {
    (void)h2_desktop_recording_stop(state, nullptr);
    h2_desktop_recording_encoder_destroy(state->encoder);
    delete state;
  }
}
} // extern "C"
