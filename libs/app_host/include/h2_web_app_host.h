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

#define H2_WEB_APP_HOST_MAX_BUTTONS 8u

/** One push-edge Button a web board offers. */
typedef struct h2_web_board_button {
  const char *name;
  /** KeyboardEvent.key that drives it, e.g. "Escape"; "" for none. */
  const char *key;
} h2_web_board_button_t;

/**
 * The web board a page runs on: the browser counterpart of a physical board.
 * h2_web_app() generates this definition from its h2_web_board() target
 * (web_board.bzl); the chosen skin supplies the page UI.
 */
typedef struct h2_web_board {
  int32_t display_width;
  int32_t display_height;
  const h2_web_board_button_t *buttons;
  size_t button_count;
} h2_web_board_t;

extern const h2_web_board_t h2_web_board;

/**
 * Maps one board Button (by name) to an App component. The shell's
 * JavaScript owns the input: it binds the key (the board key unless `key`
 * overrides it; NULL keeps the board key) and every skin element marked
 * `data-h2-button="<name>"` (pointer and touch), merges them into one pressed
 * state and pushes Down/Up edges.
 */
typedef struct h2_web_app_host_button {
  h2_runtime_component_id_t component_id;
  const char *key;
  const char *name;
} h2_web_app_host_button_t;

/**
 * Target-supplied board hardware for Apps that need more than a display and
 * Buttons (simulated power, battery, vibration, their own component ids).
 * Every hook is optional.
 */
typedef struct h2_web_app_host_hardware {
  void *user;
  /**
   * Runs before the Filesystem opens, e.g. to unpack preloaded assets into a
   * directory the Filesystem later mounts read-only.
   */
  h2_pal_result_t (*prepare)(void *user, h2_web_platform_t *platform);
  /**
   * Adjusts the Runtime configuration after the host filled its Web
   * providers, e.g. to install power, periph, input and pwm_switch APIs and a
   * component mapper. With a mapper installed the host does not add its own
   * Button peripherals; `button` must then deliver the edges.
   */
  h2_pal_result_t (*configure_runtime)(void *user, h2_runtime_config_t *config);
  /**
   * Delivers one Button edge for the mapped component (1 down, 0 up); NULL
   * pushes it on the host's own Button peripheral.
   */
  h2_pal_result_t (*button)(void *user, h2_runtime_t *runtime,
                            h2_runtime_component_id_t component_id,
                            int pressed);
} h2_web_app_host_hardware_t;

typedef struct h2_web_app_host_config {
  /** Short App name used in the markers. */
  const char *name;
  /** Zero takes the web board's display size. */
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
  /** Entry owns dependent workers and performs checked shutdown. Stop remains
   * cooperative: the 2-second task cancellation is disabled, since an EXIT
   * cannot prove its workers/resources safe to release. A stuck PAL may keep
   * shutdown pending indefinitely while the page event loop stays responsive.
   * Set only for entries that honor should_stop and check dependent cleanup. */
  int managed_shutdown;
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
  /** Optional board hardware hooks; NULL keeps the host defaults. */
  const h2_web_app_host_hardware_t *hardware;
} h2_web_app_host_config_t;

/** run result: the entire dependency graph is still retained in this module.
 * No restart/recovery API is provided; page/module teardown is the boundary. */
#define H2_WEB_APP_HOST_RETAINED 2

/** Run one App; returns 0 for complete successful teardown, 1 for an ordinary
 * failure with teardown, or RETAINED when cleanup could not prove safety.
 * Copies config, button/hardware descriptors and their strings/root lists into
 * one owned allocation. Opaque entry user, hardware user, callback/provider and
 * allocator contexts remain borrowed: they must be persistent (static/owned
 * heap), and their owner must keep them alive on RETAINED. Automatic/stack
 * callback contexts are not valid for resources that can outlive this call.
 * A retained Host owns Runtime, Filesystem, LVGL/platform and its descriptors;
 * new runs and UI input are rejected. Ordinary entry errors still tear down.
 * A retained result never claims transport cancellation or successful join. */
int h2_web_app_host_run(const h2_web_app_host_config_t *config,
                        h2_web_app_host_entry_fn entry, void *user);

/** Report a dependent cleanup failure from the App task before returning.
 * owner is the persistent failed acquisition root (e.g. a Lua Host), or NULL
 * for an outer task join failure. Repeating the same root/error is safe; a
 * different non-NULL root is rejected. Latches the first negative reason;
 * a positive non-OK status (EXIT) is normalized to ERR_TASK.
 * Does not stop/join workers, recover hardware or take ownership of arbitrary
 * callback contexts; those remain subject to run's retention precondition.
 * Use only when cleanup really cannot release resources, not for an ordinary
 * script/application error. The outer Host skips dependent teardown, reports
 * FAIL with retained=1 to the shell and keeps this root and its own full graph. */
h2_pal_result_t h2_web_app_host_quarantine(h2_web_app_host_t *host,
    void *owner, h2_pal_result_t reason);

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
