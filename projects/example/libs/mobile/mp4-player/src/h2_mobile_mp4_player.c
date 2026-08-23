#include "h2_mobile_mp4_player.h"

#include "h2_smoke_mp4_player.h"

#include <string.h>

typedef struct h2_mobile_mp4_source {
  const uint8_t *media;
  size_t media_size;
} h2_mobile_mp4_source_t;

static h2_pal_result_t h2_mobile_mp4_read_at(
    void *user, uint64_t offset, void *buffer, size_t capacity,
    size_t *out_read) {
  const h2_mobile_mp4_source_t *source = user;
  if (source == NULL || out_read == NULL ||
      (buffer == NULL && capacity != 0u) || offset > source->media_size) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const size_t start = (size_t)offset;
  const size_t remaining = source->media_size - start;
  const size_t count = capacity < remaining ? capacity : remaining;
  if (count != 0u) {
    memcpy(buffer, source->media + start, count);
  }
  *out_read = count;
  return H2_PAL_OK;
}

h2_pal_result_t h2_mobile_mp4_player_run(
    h2_runtime_t *runtime, const h2_mobile_mp4_player_config_t *config) {
  if (runtime == NULL || config == NULL || config->media == NULL ||
      config->media_size == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_mobile_mp4_source_t source = {
      .media = config->media,
      .media_size = config->media_size,
  };
  const h2_smoke_mp4_player_config_t player_config = {
      .source = {
          .user = &source,
          .size = source.media_size,
          .read_at = h2_mobile_mp4_read_at,
      },
      .media_path = NULL,
      .acquire_timeout_ms = 1000u,
      .max_frames = 0u,
      .looping = 1,
      .display_mode = H2_SMOKE_MP4_PLAYER_DISPLAY_EXACT,
      .require_audio = 1,
      .should_stop = config->should_stop,
      .stop_user = config->stop_user,
      .on_ready = NULL,
      .ready_user = NULL,
  };
  return h2_smoke_mp4_player_run(runtime, &player_config);
}
