/* Desktop preview and macOS bundle: SDL3 stays on the main thread. */
#include "h2_desktop_platform.h"
#include "h2_fruit_slice.h"
#include "h2_fruit_slice_native_host.h"
#include "h2_fruit_slice_desktop_pref.h"
#include "h2_sdl3.h"
#include <stdio.h>

typedef struct window {
  h2_sdl3_t* sdl;
  bool closed;
} window_t;

static bool should_exit(void* user) {
  window_t* window = user;
  h2_sdl3_event_t event;
  while (h2_sdl3_poll_event(window->sdl, &event) == H2_PAL_OK) {
    if (event.kind == H2_SDL3_EVENT_CLOSE ||
        (event.kind == H2_SDL3_EVENT_KEY && event.pressed &&
         event.key == H2_SDL3_KEY_ESCAPE))
      window->closed = true;
  }
  return window->closed;
}

static h2_runtime_config_t runtime_config(h2_sdl3_t* sdl,
                                          h2_sqlite_t* preferences) {
  h2_runtime_config_t config = h2_fruit_slice_native_defaults();
  config.board = "desktop";
  config.target = "desktop";
  config.chip = "host";
  config.mem = h2_desktop_platform_default_allocator();
  config.log = h2_desktop_platform_log_api();
  config.time = h2_desktop_platform_time_api();
  config.timer = h2_desktop_platform_timer_api();
  config.task = h2_desktop_platform_task_api();
  config.queue = h2_desktop_platform_queue_api();
  config.sync = h2_desktop_platform_sync_api();
  config.display = h2_sdl3_display(sdl);
  config.touch = h2_sdl3_touch(sdl);
  config.pref = h2_sqlite_pref_api(preferences);
  return config;
}

int main(int argc, char** argv) {
  int width = 480, height = 480;
  if (argc > 2 ||
      (argc == 2 && sscanf(argv[1], "%dx%d", &width, &height) != 2)) {
    fprintf(stderr, "usage: %s [WIDTHxHEIGHT]\n", argv[0]);
    return 2;
  }
  h2_sqlite_t* preferences = NULL;
  int rc = h2_fruit_slice_desktop_pref_create(NULL, &preferences);
  if (rc != H2_PAL_OK) {
    fprintf(stderr, "fruit-slice: persistent Preferences failed rc=%d\n", rc);
    return 1;
  }
  const h2_sdl3_config_t surface = {
      .title = "Fruit Slice", .width = width, .height = height};
  window_t window = {0};
  if (h2_sdl3_create(&surface, &window.sdl) != H2_PAL_OK) {
    fprintf(stderr, "fruit-slice: SDL3 window failed\n");
    h2_sqlite_destroy(preferences);
    return 1;
  }
  const h2_runtime_config_t config = runtime_config(window.sdl, preferences);
  h2_runtime_t* runtime = NULL;
  rc = h2_runtime_init(&config, &runtime);
  if (rc == H2_PAL_OK) {
    const h2_fruit_slice_config_t game = {.should_exit = should_exit,
                                          .user = &window};
    rc = h2_fruit_slice_run(runtime, &game);
  }
  const int destroyed = runtime != NULL ? h2_runtime_destroy(&runtime) : 0;
  if (destroyed == H2_PAL_OK) {
    h2_sdl3_destroy(window.sdl);
    h2_sqlite_destroy(preferences);
  }
  if (rc != H2_PAL_OK || destroyed != H2_PAL_OK) {
    fprintf(stderr, "fruit-slice: rc=%d destroy=%d\n", rc, destroyed);
    return 1;
  }
  return 0;
}
