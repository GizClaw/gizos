#include "h2_smoke_host_runtime.h"
#include "h2_smoke_mp4_player.h"
#include "h2_web_platform.h"

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct web_media {
  uint8_t *data;
  size_t size;
} web_media_t;

typedef struct web_player {
  h2_runtime_t *runtime;
  h2_smoke_mp4_player_config_t config;
  h2_pal_result_t result;
  int done;
} web_player_t;

EM_JS(void, web_set_status, (const char *status), {
  const element = document.getElementById('status');
  if (element)
    element.textContent = UTF8ToString(status);
});

EM_ASYNC_JS(void, web_wait_for_start, (), {
  const button = document.getElementById('start');
  if (!button)
    return;
  await new Promise(
      resolve => button.addEventListener('click', resolve, {once : true}));
  button.disabled = true;
  button.textContent = 'Playing';
});

static h2_pal_result_t web_media_read_at(void *user, uint64_t offset,
                                         void *buffer, size_t capacity,
                                         size_t *out_read) {
  const web_media_t *media = user;
  if (media == NULL || out_read == NULL || offset > media->size ||
      (buffer == NULL && capacity != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  size_t count = media->size - (size_t)offset;
  if (count > capacity)
    count = capacity;
  if (count != 0u)
    memcpy(buffer, media->data + (size_t)offset, count);
  *out_read = count;
  return H2_PAL_OK;
}

static int web_load_media(const char *path, web_media_t *media) {
  FILE *file = fopen(path, "rb");
  if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
    if (file != NULL)
      (void)fclose(file);
    return 0;
  }
  const long length = ftell(file);
  if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
    (void)fclose(file);
    return 0;
  }
  uint8_t *data = malloc((size_t)length);
  if (data == NULL || fread(data, 1u, (size_t)length, file) != (size_t)length) {
    free(data);
    (void)fclose(file);
    return 0;
  }
  (void)fclose(file);
  *media = (web_media_t){.data = data, .size = (size_t)length};
  return 1;
}

static void web_player_task(void *context) {
  web_player_t *player = context;
  player->result = h2_smoke_mp4_player_run(player->runtime, &player->config);
  player->done = 1;
}

int main(void) {
  const h2_web_platform_config_t platform_config = {
      .display_width = 240,
      .display_height = 240,
  };
  h2_web_platform_t *platform = h2_web_platform_create(&platform_config);
  if (platform == NULL) {
    web_set_status("Web PAL initialization failed");
    return 1;
  }
  web_set_status("Ready — press Play to enable audio");
  web_wait_for_start();

  web_media_t media = {0};
  if (!web_load_media("/media/startup.mp4", &media)) {
    web_set_status("Embedded MP4 could not be loaded");
    h2_web_platform_destroy(platform);
    return 1;
  }
  h2_runtime_config_t runtime_config = h2_smoke_host_runtime_config(
      "browser", "webassembly", "wasm32", h2_web_platform_mem_api(),
      h2_web_platform_time_api(platform), h2_web_platform_queue_api(platform),
      h2_web_platform_display_api(platform));
  runtime_config.log = h2_web_platform_log_api();
  runtime_config.task = h2_web_platform_task_api(platform);
  runtime_config.sync = h2_web_platform_sync_api(platform);
  runtime_config.audio = h2_web_platform_audio_api(platform);
  runtime_config.audio_decoder = h2_web_platform_audio_decoder_api(platform);
  runtime_config.video_decoder = h2_web_platform_video_decoder_api(platform);

  h2_runtime_t *runtime = NULL;
  h2_pal_result_t result = h2_runtime_init(&runtime_config, &runtime);
  if (result == H2_PAL_OK) {
    web_player_t player = {
        .runtime = runtime,
        .config =
            {
                .source = {.user = &media,
                           .size = media.size,
                           .read_at = web_media_read_at},
                .acquire_timeout_ms = 1000u,
                .display_mode = H2_SMOKE_MP4_PLAYER_DISPLAY_EXACT,
        .require_audio = 1,
            },
        .result = H2_PAL_ERR_TASK,
    };
    web_set_status("Decoding with browser WebCodecs");
    h2_pal_task_t *task = NULL;
    const h2_pal_task_options_t task_options = {
        .name = "web-mp4-player",
        .min_stack_size = 65536u,
    };
    result = h2_pal_task_start(runtime->task, &task_options, web_player_task,
                               &player, &task);
    while (result == H2_PAL_OK && !player.done) {
      result = h2_web_platform_pump(platform, 32u, NULL);
      if (result == H2_PAL_OK && !player.done)
        emscripten_sleep(1u);
    }
    if (result == H2_PAL_OK)
      result = player.result;
    if (task != NULL) {
      const h2_pal_result_t joined = h2_pal_task_join(runtime->task, task);
      if (result == H2_PAL_OK)
        result = joined;
    }
  }
  if (runtime != NULL)
    h2_runtime_deinit(runtime);
  free(media.data);
  h2_web_platform_destroy(platform);
  web_set_status(result == H2_PAL_OK ? "Playback complete"
                                     : "Playback failed — see console");
  return result == H2_PAL_OK ? 0 : 1;
}
