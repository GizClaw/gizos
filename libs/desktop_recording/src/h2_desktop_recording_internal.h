#ifndef H2_DESKTOP_RECORDING_INTERNAL_H
#define H2_DESKTOP_RECORDING_INTERNAL_H
#include "h2_desktop_recording.h"

struct h2_desktop_recording_encoder;
using h2_desktop_recording_encoder_t = h2_desktop_recording_encoder;
struct h2_desktop_recording_encoder_config {
  const char *path;
  uint32_t width;
  uint32_t height;
  uint64_t start_us;
  // Private integration-test sink: duplicate a borrowed descriptor to exercise
  // blocked/broken writes without replacing the real encoder or POSIX I/O.
  // Public creation always leaves this at -1 and exclusively creates path.
  int output_fd = -1;
};
using h2_desktop_recording_encoder_config_t =
    h2_desktop_recording_encoder_config;
uint64_t h2_desktop_recording_now_us();
h2_pal_result_t h2_desktop_recording_encoder_result(
    h2_desktop_recording_encoder_t *recording);
h2_pal_result_t h2_desktop_recording_encoder_create(
    const h2_desktop_recording_encoder_config_t *config,
    h2_desktop_recording_encoder_t **out_recording);
void h2_desktop_recording_encoder_video(void *user,
                                        const h2_sdl3_capture_frame_t *frame);
void h2_desktop_recording_encoder_audio(
    void *user, const h2_portaudio_capture_frame_t *frame);
h2_pal_result_t
h2_desktop_recording_encoder_finish(h2_desktop_recording_encoder_t *recording,
                                    uint64_t stop_us,
                                    h2_desktop_recording_stats_t *out_stats);
void h2_desktop_recording_encoder_destroy(
    h2_desktop_recording_encoder_t *recording);
#endif
