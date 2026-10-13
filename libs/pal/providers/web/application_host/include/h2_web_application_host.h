#ifndef H2_WEB_APPLICATION_HOST_H
#define H2_WEB_APPLICATION_HOST_H

#include "h2_application_host.h"
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
 * Audio, decoders, HTTP, WebRTC, Netif, System Event, Pref, Crypto, Timer,
 * and LED output for board-owned Periph descriptors)
 * and the canonical unsupported APIs elsewhere. Explicit
 * Module.h2WebEnvironment configuration opts into fake Wi-Fi/settings/modem
 * (see the Web guide). Missing configuration keeps radios unsupported;
 * simulated radio state never fabricates a server connection. The host runs the
 * App entry in a task, pumps until it returns, and tears everything down.
 * Progress and the result go to the console and #status as H2_WEB_APP
 * name=APP_NAME stage=running H2_WEB_APP name=APP_NAME result=PASS rc=0 fs=0
 * destroy=0 where PASS additionally requires a complete teardown (Filesystem
 * closed and platform destroyed with nothing left pinned).
 */
typedef h2_application_host_entry_fn h2_web_application_host_entry_fn;

#define H2_WEB_APPLICATION_HOST_MAX_BUTTONS 8u

/** One push-edge Button a web board offers. */
typedef struct h2_web_application_board_button {
    const char *name;
    /** KeyboardEvent.key that drives it, e.g. "Escape"; "" for none. */
    const char *key;
} h2_web_application_board_button_t;

/**
 * The web board a page runs on: the browser counterpart of a physical board.
 * h2_web_app() generates this definition from its h2_web_board() target
 * (web_board.bzl); the chosen skin supplies the page UI.
 */
typedef struct h2_web_application_board {
    int32_t display_width;
    int32_t display_height;
    const h2_web_application_board_button_t *buttons;
    size_t button_count;
} h2_web_application_board_t;

extern const h2_web_application_board_t h2_web_application_board;

/**
 * Maps one board Button (by name) to an App component. The shell's
 * JavaScript owns the input: it binds the key (the board key unless `key`
 * overrides it; NULL keeps the board key) and every skin element marked
 * `data-h2-button="APP_NAME"` (pointer and touch), merges them into one pressed
 * state and pushes Down/Up edges.
 */
typedef struct h2_web_application_host_button {
    h2_runtime_component_id_t component_id;
    const char *key;
    const char *name;
} h2_web_application_host_button_t;

/**
 * Target-supplied board hardware for Apps that need more than a display and
 * Buttons (simulated power, battery, vibration, their own component ids).
 * Every hook is optional.
 */
typedef struct h2_web_application_host_hardware {
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
    h2_pal_result_t (*configure_runtime)(void *user,
                                         h2_runtime_config_t *config);
    /** Validate product required components before external input starts. */
    h2_pal_result_t (*validate_runtime)(void *user, h2_runtime_t *runtime);
    /** Release prepare-owned resources during platform rollback, including FS
     * or Runtime setup failure. Accept partial prepare; failure retains
     * context. */
    h2_pal_result_t (*release_prepare)(void *user);
    /**
     * Delivers one Button edge for the mapped component (1 down, 0 up); NULL
     * pushes it on the host's own Button peripheral.
     */
    h2_pal_result_t (*button)(void *user, h2_runtime_t *runtime,
                              h2_runtime_component_id_t component_id,
                              int pressed);
    /**
     * Releases resources borrowed by Runtime configuration, after App and
     * Runtime users stop and before Filesystem/platform teardown. Runs once on
     * the host task, including after failed configure_runtime or Runtime init.
     * Not called if the dependency graph was already quarantined, or host setup
     * failed before Runtime configuration. Failure quarantines the graph and
     * retains user/dependencies; it is not retried. Keep user alive on
     * RETAINED.
     */
    h2_pal_result_t (*release_runtime)(void *user);
} h2_web_application_host_hardware_t;

typedef struct h2_web_application_host_config {
    /** Short App name used in the markers. */
    const char *name;
    /** Zero takes the web board's display size. */
    int32_t display_width;
    int32_t display_height;
    /** Optional writable IndexedDB root, e.g. "/data"; NULL keeps fs
     * unsupported. */
    const char *persistent_root;
    const char *const *readonly_roots;
    size_t readonly_root_count;
    /** Nonzero requests cooperative stop after this duration; entry must
     * honor h2_application_host_should_stop and join dependent workers. */
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
    const h2_web_application_host_button_t *buttons;
    size_t button_count;
    /** App task stack bytes; zero selects 64 KiB. */
    size_t stack_size;
    /** Optional board hardware hooks; NULL keeps the host defaults. */
    const h2_web_application_host_hardware_t *hardware;
} h2_web_application_host_config_t;

/** run result: the lifetime root and any still-live dependencies are retained.
 * No restart/recovery API is provided; page/module teardown is the boundary. */
#define H2_WEB_APPLICATION_HOST_RETAINED 2

/** Run one App; returns 0 for complete successful teardown, 1 for an ordinary
 * failure with teardown, or RETAINED when cleanup could not prove safety.
 * Copies config, button/hardware descriptors and their strings/root lists into
 * one owned allocation. Opaque entry user, hardware user, callback/provider and
 * allocator contexts remain borrowed: they must be persistent (static/owned
 * heap), and their owner must keep them alive on RETAINED. Automatic/stack
 * callback contexts are not valid for resources that can outlive this call.
 * A retained Host keeps its descriptors and dependencies that have not safely
 * finished cleanup; already released resources are not usable.
 * new runs and UI input are rejected. Ordinary entry errors still tear down.
 * A retained result never claims transport cancellation or successful join. */
int h2_web_application_host_run(const h2_web_application_host_config_t *config,
                                h2_web_application_host_entry_fn entry,
                                void *user);

/** Borrow the Web platform from a Host created by this provider. */
h2_web_platform_t *
h2_web_application_host_platform(h2_application_host_t *host);

#ifdef __cplusplus
}
#endif

#endif
