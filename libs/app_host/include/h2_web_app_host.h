#ifndef H2_WEB_APP_HOST_H
#define H2_WEB_APP_HOST_H

#include "h2_runtime.h"
#include "h2_web_platform.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Browser launcher for one portable App.
 *
 * Creates the Web platform, optionally the persistent Filesystem and the LVGL
 * platform, assembles a Runtime with every Web provider (Display, Touch,
 * Audio, decoders, HTTP, WebRTC, Netif, System Event, Pref, Crypto, Timer)
 * and the canonical unsupported APIs elsewhere, runs the App entry in a task,
 * pumps until it returns, and tears everything down. Progress and the result
 * go to the console and #status as
 *   H2_WEB_APP name=<name> stage=running
 *   H2_WEB_APP name=<name> result=PASS rc=0 fs=0 destroy=0
 * where PASS additionally requires a complete teardown (Filesystem closed
 * and platform destroyed with nothing left pinned).
 */
typedef struct h2_web_app_host h2_web_app_host_t;

typedef h2_pal_result_t (*h2_web_app_host_entry_fn)(h2_web_app_host_t *host,
                                                    h2_runtime_t *runtime,
                                                    void *user);

/**
 * One Runtime Button driven from the page. The shell's JavaScript owns the
 * input: it binds `key` (KeyboardEvent.key, e.g. "Escape"; NULL for none) and
 * every element marked `data-h2-button="<name>"` in the page layout (pointer
 * and touch; NULL name for none), merges them into one pressed state and
 * pushes Down/Up edges. Layout is plain HTML/CSS supplied through
 * h2_web_app(layout = ...), usually a board's reusable HTML/CSS.
 */
typedef struct h2_web_app_host_button {
  h2_runtime_component_id_t component_id;
  const char *key;
  const char *name;
} h2_web_app_host_button_t;

#define H2_WEB_APP_HOST_MAX_BUTTONS 8u

typedef struct h2_web_app_host_config {
  /** Short App name used in the markers. */
  const char *name;
  int32_t display_width;
  int32_t display_height;
  /** Optional writable IndexedDB root, e.g. "/data"; NULL keeps fs unsupported. */
  const char *persistent_root;
  const char *const *readonly_roots;
  size_t readonly_root_count;
  /**
   * Nonzero requests a stop after this long (endless Apps): should_stop turns
   * true, and 2 s later the App task is cancelled so its next PAL wait
   * returns EXIT, which then counts as a clean stop.
   */
  uint32_t run_ms;
  /** Nonzero initializes the LVGL platform for LVGL-based Apps. */
  int lvgl;
  /**
   * Optional push-edge Buttons. The host exposes them as single-button
   * peripherals with a component mapper, starts Runtime input and hands the
   * table to the page, whose keyboard and layout elements push
   * h2_runtime_button_push_edge() edges; click and long-press remain Runtime
   * decisions.
   */
  const h2_web_app_host_button_t *buttons;
  size_t button_count;
  /** App task stack bytes; zero selects 64 KiB. */
  size_t stack_size;
} h2_web_app_host_config_t;

/** Run the App to completion; returns 0 for PASS and 1 otherwise. */
int h2_web_app_host_run(const h2_web_app_host_config_t *config,
                        h2_web_app_host_entry_fn entry, void *user);

/**
 * should_stop callback for Apps: nonzero once run_ms elapsed or the page
 * called Module._h2_web_app_host_request_stop(). Pass the host as user.
 */
int h2_web_app_host_should_stop(void *host);

/** Mark that the App reached its ready point (prints stage=ready). */
h2_pal_result_t h2_web_app_host_ready(void *host);

h2_web_platform_t *h2_web_app_host_platform(h2_web_app_host_t *host);

#ifdef __cplusplus
}
#endif

#endif
