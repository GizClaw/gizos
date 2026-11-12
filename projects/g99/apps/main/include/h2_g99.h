#ifndef H2_G99_H
#define H2_G99_H

/**
 * @file h2_g99.h
 * @brief Portable G99 (GizPocket) Main entry, logical inputs and observation.
 *
 * Main draws its system UI with LVGL directly on Runtime Display and hands the
 * display to the Lua host while an app runs. It reads only Runtime services;
 * boards and launchers map physical keys to the logical components below.
 */

#include "hal/h2_pal_wifi.h"
#include "hal/h2_pal_touch.h"
#include "h2_runtime.h"
#include "h2_runtime_custom_event.h"
#include "h2_g99_storage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Logical Button components. Launchers map each to one board periph. */
enum {
  /** Back / home. On Mosaico this is the AI key. Apps cannot intercept it. */
  H2_G99_COMPONENT_HOME = 1,
  /** Screen off/on toggle. On Mosaico this is BOOT; on BK7258 POWER short. */
  H2_G99_COMPONENT_SCREEN = 2,
  H2_G99_COMPONENT_UP = 3,
  H2_G99_COMPONENT_DOWN = 4,
  H2_G99_COMPONENT_OK = 5,
};

/** Optional Battery component; an unmapped battery shows no percentage. */
#define H2_G99_COMPONENT_BATTERY 40u
/** Optional PWM-switch component for short vibration feedback pulses. */
#define H2_G99_COMPONENT_HAPTICS 41u

/** Owner of G99 Runtime custom events. */
#define H2_G99_CUSTOM_EVENT_OWNER 0x0099u

/**
 * Logical text input: a Runtime custom event whose payload is UTF-8 text
 * without a terminator. While the Wi-Fi password keyboard is open, Main
 * replaces the typed text with it; at other times Main ignores it. A launcher
 * or test posts it with h2_runtime_post_custom_event(), the way a hardware
 * keyboard or IME would enter text without touching the on-screen keys.
 */
#define H2_G99_EVENT_TEXT_INPUT \
  H2_RUNTIME_CUSTOM_EVENT_ID(H2_G99_CUSTOM_EVENT_OWNER, 1u)

typedef enum h2_g99_lifecycle {
  H2_G99_LIFECYCLE_STARTING = 0,
  H2_G99_LIFECYCLE_RUNNING,
  H2_G99_LIFECYCLE_APP_RUNNING,
  H2_G99_LIFECYCLE_STOPPING,
  H2_G99_LIFECYCLE_STOPPED,
  H2_G99_LIFECYCLE_FAILED,
} h2_g99_lifecycle_t;

typedef enum h2_g99_route {
  H2_G99_ROUTE_BOOT = 0,
  H2_G99_ROUTE_MAIN,
  H2_G99_ROUTE_LIBRARY,
  H2_G99_ROUTE_SETTINGS,
  H2_G99_ROUTE_APP,
} h2_g99_route_t;

typedef enum h2_g99_transition {
  H2_G99_TRANSITION_NONE = 0,
  H2_G99_TRANSITION_BOOT,
  H2_G99_TRANSITION_ENTER,
  H2_G99_TRANSITION_BACK,
  H2_G99_TRANSITION_ROTATE,
} h2_g99_transition_t;

/** Main menu entries in wheel order. */
typedef enum h2_g99_main_item {
  H2_G99_MAIN_ME = 0,
  H2_G99_MAIN_STORE,
  H2_G99_MAIN_LIBRARY,
  H2_G99_MAIN_LAB,
  H2_G99_MAIN_SETTINGS,
  H2_G99_MAIN_COUNT,
} h2_g99_main_item_t;

/** Stable Settings identifiers. INPUT is retained for compatibility and omitted
 * from the wheel. Wi-Fi, volume, brightness, timeout, About, haptics and
 * storage are implemented. */
typedef enum h2_g99_setting_item {
  H2_G99_SETTING_WIFI = 0,
  H2_G99_SETTING_VOLUME,
  H2_G99_SETTING_BRIGHTNESS,
  H2_G99_SETTING_SCREEN_TIMEOUT,
  H2_G99_SETTING_ABOUT,
  H2_G99_SETTING_HAPTICS,
  H2_G99_SETTING_INPUT,
  H2_G99_SETTING_STORAGE,
  H2_G99_SETTING_UPDATE,
  H2_G99_SETTING_ACCOUNT,
  H2_G99_SETTING_CLOCK,
  H2_G99_SETTING_LANGUAGE,
  H2_G99_SETTING_RESET,
  H2_G99_SETTING_COUNT,
} h2_g99_setting_item_t;

/** Wi-Fi activity reported by Main's Wi-Fi worker. */
typedef enum h2_g99_wifi_status {
  /** The target has no usable Wi-Fi station. */
  H2_G99_WIFI_STATUS_UNAVAILABLE = 0,
  H2_G99_WIFI_STATUS_IDLE,
  H2_G99_WIFI_STATUS_SCANNING,
  H2_G99_WIFI_STATUS_CONNECTING,
} h2_g99_wifi_status_t;

/** Screen rectangle in pixels; x1/y1 inclusive, x2/y2 exclusive. */
typedef struct h2_g99_rect {
  int32_t x1;
  int32_t y1;
  int32_t x2;
  int32_t y2;
} h2_g99_rect_t;

/** Public state snapshot for host and product E2E assertions. */
typedef struct h2_g99_observation {
  /** Increments on every published change. */
  uint32_t sequence;
  h2_g99_lifecycle_t lifecycle;
  h2_g99_route_t route;
  /** Route that becomes current when the transition finishes. */
  h2_g99_route_t target_route;
  h2_g99_transition_t transition;
  /** Main wheel selection, an h2_g99_main_item_t. */
  uint8_t main_selection;
  /** Library or Settings wheel selection; zero on MAIN. */
  uint8_t sub_selection;
  /** True while a settings item is being edited. */
  bool editing;
  /** Focused row inside the edited settings item. */
  uint8_t edit_index;
  /** True when UI input is ready (no boot or page transition running).
   * An open Library drawer captures input before background navigation. */
  bool input_ready;
  bool screen_on;
  uint8_t volume_percent;
  uint8_t brightness_percent;
  /** Index into the screen timeout choices, see h2_g99_settings.h. */
  uint8_t screen_timeout_index;
  int32_t width;
  int32_t height;
  /** True when Main read its settings store, including an empty one. */
  bool settings_loaded;
  /** True while the Wi-Fi password keyboard is open. */
  bool text_entry;
  h2_g99_wifi_status_t wifi_status;
  bool wifi_connected;
  /** Connected network, empty when not connected. */
  char wifi_ssid[H2_PAL_WIFI_SSID_MAX + 1];
  /** Networks in the last scan list. */
  uint8_t wifi_network_count;
  /** Network on the focused row while the Wi-Fi list is edited, else empty. */
  char wifi_focused_ssid[H2_PAL_WIFI_SSID_MAX + 1];
  /** Result of the last finished scan or connect attempt. */
  int32_t wifi_last_result;
  /** Layout at rest: visible wheel slots at full size including tilt, or
   * Library's left screenshot area, and the information area. */
  h2_g99_rect_t wheel_rect;
  h2_g99_rect_t info_rect;
  /** Items a finger has turned the wheel past the selection while it drags
   * the wheel (positive brings the next items up); zero otherwise. When the
   * finger lifts, the wheel turns on to the nearest item. */
  float wheel_drag;
} h2_g99_observation_t;

typedef void (*h2_g99_observe_fn)(void* user,
                                  const h2_g99_observation_t* observation);

/** Timing of one frame Main presented. Durations are in microseconds. */
typedef struct h2_g99_frame_stats {
  /** Frames presented since Main started, this one included. */
  uint32_t index;
  /** Time since the previous presented frame; zero for the first. */
  uint32_t interval_us;
  /** Main loop work for this frame: placing the animation, LVGL rendering
   * and the display transfer. */
  uint32_t work_us;
  /** Parts of work_us: the display draw calls and the display present. */
  uint32_t draw_us;
  uint32_t present_us;
  /** Screen area LVGL redrew for this frame, in pixels. */
  uint32_t pixels;
} h2_g99_frame_stats_t;

typedef void (*h2_g99_frame_fn)(void* user, const h2_g99_frame_stats_t* frame);

typedef struct h2_g99_config {
  /** Human-readable firmware facts shown in Settings > About. */
  const char* main_version;
  const char* loader_version;
  const char* board_name;
  /** Returns nonzero when the launcher asks Main to exit. */
  int (*should_stop)(void* user);
  void* should_stop_user;
  /** Optional observer called from the Main task after each change. */
  h2_g99_observe_fn observe;
  void* observe_user;
  /** Preference namespace for user settings; NULL selects "g99". E2E runs use
   * their own namespace so production settings stay untouched. */
  const char* settings_namespace;
  /** Optional touch source used instead of Runtime touch, in the same screen
   * coordinates; E2E runs pass a scripted pointer. Borrowed; Main opens and
   * closes it as it would Runtime touch. */
  const h2_pal_touch_api_t* touch;
  /** Optional frame timing observer, called from the Main task after each
   * frame Main presented. */
  h2_g99_frame_fn frame;
  void* frame_user;
  /** Optional allocator for the bounded RGB565 render buffer; NULL uses
   * Runtime mem. The buffer uses up to 16 rows, falling back to eight when
   * fragmented. LVGL objects, fonts and artwork retain Runtime mem so this
   * optimization cannot exhaust an internal-only heap. Borrowed through run().
   */
  const h2_pal_mem_api_t* render_mem;
  /** Optional target-owned, read-only storage statistics provider. Copied into
   * the storage worker; user and query remain borrowed until it joins. */
  const h2_g99_storage_api_t* storage;
} h2_g99_config_t;

/**
 * Runs Main until should_stop reports nonzero or a fatal error occurs.
 *
 * Requires Runtime display, time, task, sync, queue and mem. Runtime input
 * must already be started. Touch (config->touch, else Runtime touch), Wi-Fi
 * STA, pref and audio are optional; missing services leave their features
 * unavailable. The display is borrowed: Main opens it when needed and never
 * closes a display it did not open.
 */
h2_pal_result_t h2_g99_run(h2_runtime_t* runtime,
                           const h2_g99_config_t* config);

#ifdef __cplusplus
}
#endif

#endif
