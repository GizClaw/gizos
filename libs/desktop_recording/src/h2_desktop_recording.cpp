#include "h2_desktop_recording_internal.h"

#include <new>

struct h2_desktop_recording {
  h2_desktop_recording_encoder_t *encoder = nullptr;
  h2_desktop_capture_hooks_t hooks = {};
};

extern "C" {
h2_pal_result_t
h2_desktop_recording_create(const char *path, uint32_t width, uint32_t height,
                            h2_desktop_recording_t **out_recording) {
  if (out_recording == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_recording = nullptr;
  auto *state = new (std::nothrow) h2_desktop_recording_t();
  if (state == nullptr) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  const h2_desktop_recording_encoder_config_t config = {
      path, width, height, h2_desktop_recording_now_us()};
  const h2_pal_result_t result =
      h2_desktop_recording_encoder_create(&config, &state->encoder);
  if (result != H2_PAL_OK) {
    delete state;
    return result;
  }
  state->hooks = {state->encoder, h2_desktop_recording_encoder_video, nullptr,
                  h2_desktop_recording_encoder_audio};
  *out_recording = state;
  return H2_PAL_OK;
}

const h2_desktop_capture_hooks_t *
h2_desktop_recording_hooks(h2_desktop_recording_t *state) {
  return state == nullptr ? nullptr : &state->hooks;
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
}
