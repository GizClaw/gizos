#include "h2_desktop_capture.h"

#include <new>

struct h2_desktop_capture {
  h2_sdl3_t *display = nullptr;
  h2_portaudio_t *audio = nullptr;
};

extern "C" {
h2_pal_result_t
h2_desktop_capture_create(const h2_desktop_capture_config_t *config,
                          h2_desktop_capture_t **out_capture) {
  if (out_capture == nullptr) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_capture = nullptr;
  if (config == nullptr ||
      (config->hooks.on_display == nullptr && config->hooks.on_mic == nullptr &&
       config->hooks.on_speaker == nullptr)) {
    return H2_PAL_OK;
  }
  const bool with_audio =
      config->hooks.on_mic != nullptr || config->hooks.on_speaker != nullptr;
  if ((config->hooks.on_display != nullptr && config->display == nullptr) ||
      (with_audio && config->audio == nullptr)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  auto *capture = new (std::nothrow) h2_desktop_capture_t();
  if (capture == nullptr) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  h2_pal_result_t result = H2_PAL_OK;
  if (config->hooks.on_display != nullptr) {
    result = h2_sdl3_set_frame_capture(
        config->display, config->hooks.on_display, config->hooks.user);
    if (result == H2_PAL_OK) {
      capture->display = config->display;
    }
  }
  if (result == H2_PAL_OK && with_audio) {
    const h2_portaudio_capture_hooks_t hooks = {
        config->hooks.user, config->hooks.on_mic, config->hooks.on_speaker};
    result = h2_portaudio_set_capture_hooks(config->audio, &hooks);
    if (result == H2_PAL_OK) {
      capture->audio = config->audio;
    }
  }
  if (result != H2_PAL_OK) {
    h2_desktop_capture_destroy(capture);
    return result;
  }
  *out_capture = capture;
  return H2_PAL_OK;
}

void h2_desktop_capture_destroy(h2_desktop_capture_t *capture) {
  if (capture == nullptr) {
    return;
  }
  if (capture->display != nullptr) {
    (void)h2_sdl3_set_frame_capture(capture->display, nullptr, nullptr);
  }
  if (capture->audio != nullptr) {
    (void)h2_portaudio_set_capture_hooks(capture->audio, nullptr);
  }
  delete capture;
}
}
