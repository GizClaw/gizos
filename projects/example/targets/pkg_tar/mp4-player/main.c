#include "h2_web_application_host.h"
#include "h2_smoke_mp4_player.h"
#include "h2_web_main_thread.h"

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct web_media {
  uint8_t *data;
  size_t size;
} web_media_t;

// The large-media archive overrides the display to the video's size.
#ifndef H2_WEB_MP4_WIDTH
#define H2_WEB_MP4_WIDTH 240
#endif
#ifndef H2_WEB_MP4_HEIGHT
#define H2_WEB_MP4_HEIGHT 240
#endif

/* clang-format off */
EM_JS(void, web_set_status,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["pointer"], null,
    (status) => {
  const element = document.getElementById('status');
  if (element)
    element.textContent = UTF8ToString(status);
});
});
/* clang-format on */

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

static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
  (void)user;
  (void)h2_web_main_call(web_set_status, (const void *[]){&(const char *){
                                             "Loading embedded MP4"}});

  web_media_t media = {0};
  if (!web_load_media("/media/startup.mp4", &media)) {
    (void)h2_web_main_call(
        web_set_status,
        (const void *[]){&(const char *){"Embedded MP4 could not be loaded"}});
    return H2_PAL_ERR_IO;
  }
  const h2_smoke_mp4_player_config_t config = {
      .source = {.user = &media, .size = media.size, .read_at = web_media_read_at},
      .acquire_timeout_ms = 1000u, .display_mode = H2_SMOKE_MP4_PLAYER_DISPLAY_EXACT,
      .require_audio = 1, .should_stop = h2_application_host_should_stop, .stop_user = host};
  (void)h2_web_main_call(web_set_status, (const void *[]){&(const char *){"Decoding with browser WebCodecs"}});
  h2_application_host_ready(host);
  int rc = h2_smoke_mp4_player_run(runtime, &config);
  free(media.data);
  (void)h2_web_main_call(web_set_status, (const void *[]){&(const char *){rc == H2_PAL_OK ? "Playback complete" : "Playback failed - see console"}});
  return rc;
}
int main(void) {
  const h2_web_application_host_config_t config = {
      .name = "mp4-player", .display_width = H2_WEB_MP4_WIDTH, .display_height = H2_WEB_MP4_HEIGHT};
  return h2_web_application_host_run(&config, run, NULL);
}
