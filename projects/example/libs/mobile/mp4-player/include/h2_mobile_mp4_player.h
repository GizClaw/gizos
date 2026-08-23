#ifndef H2_MOBILE_MP4_PLAYER_H
#define H2_MOBILE_MP4_PLAYER_H

#include "h2_runtime.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Immutable Mobile MP4 Player input borrowed for the blocking call. */
typedef struct h2_mobile_mp4_player_config {
  /** Complete MP4 asset retained by the caller during playback. */
  const uint8_t *media;
  /** Size of media in bytes; must be nonzero. */
  size_t media_size;
  /** Optional cooperative lifecycle callback polled during playback. */
  int (*should_stop)(void *user);
  /** Borrowed callback context retained for the blocking call. */
  void *stop_user;
} h2_mobile_mp4_player_config_t;

/**
 * @brief Run the looping Mobile audio/video presentation of MP4 Player.
 * @param runtime Borrowed Runtime with Memory, Time, Task, Queue, Display,
 * Audio, Audio Decoder, and Video Decoder capabilities.
 * @param config Borrowed media and lifecycle configuration.
 * @return H2_PAL_OK after cooperative stop, otherwise the playback failure.
 */
h2_pal_result_t h2_mobile_mp4_player_run(
    h2_runtime_t *runtime, const h2_mobile_mp4_player_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
