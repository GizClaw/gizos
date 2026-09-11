#include "h2_smoke_audio_system.h"
#include "h2_web_app_host.h"

#include <stdio.h>

static const char *const k_readonly_roots[] = {"/assets"};

/*
 * The scene runs its own music and microphone tasks; the entry keeps the
 * Runtime alive until the host asks to stop, then joins them.
 */
static h2_pal_result_t run_audio_system(h2_web_app_host_t *host,
                                        h2_runtime_t *runtime, void *user) {
  (void)user;
  const h2_smoke_audio_system_config_t config = {
      .music_path = "/assets/audio/music_loop.ogg",
  };
  int rc = h2_smoke_audio_system_run(runtime, &config);
  printf("H2_WEB_AUDIO_SYSTEM run rc=%d\n", rc);
  if (rc == H2_AUDIO_OK)
    (void)h2_web_app_host_ready(host);
  while (rc == H2_AUDIO_OK && !h2_web_app_host_should_stop(host))
    rc = h2_pal_time_sleep_ms(runtime->time, 20u);
  const int stop_rc = h2_smoke_audio_system_stop();
  printf("H2_WEB_AUDIO_SYSTEM stop rc=%d\n", stop_rc);
  return (h2_pal_result_t)(rc != H2_AUDIO_OK ? rc : stop_rc);
}

int main(void) {
  const h2_web_app_host_config_t config = {
      .name = "audio-system",
      .display_width = 240,
      .display_height = 240,
      .persistent_root = "/data",
      .readonly_roots = k_readonly_roots,
      .readonly_root_count = 1u,
      .run_ms = 4000u,
  };
  return h2_web_app_host_run(&config, run_audio_system, NULL);
}
