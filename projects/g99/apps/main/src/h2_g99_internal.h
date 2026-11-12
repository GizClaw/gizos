#ifndef H2_G99_INTERNAL_H
#define H2_G99_INTERNAL_H

#include "h2_g99.h"
#include "h2_g99_art.h"
#include "h2_g99_layout.h"
#include "h2_g99_nav.h"
#include "h2_g99_settings.h"
#include "h2_g99_touch.h"
#include "h2_g99_wifi.h"
#include "h2_g99_haptics.h"
#include "h2_g99_storage_worker.h"
#include "h2_lua.h"
#include "h2_lvgl_touch.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_G99_WHEEL_SLOTS 7
#define H2_G99_LIBRARY_COUNT 1u
#define H2_G99_BUTTON_COUNT 6u

typedef struct h2_g99_image {
  lv_image_dsc_t dsc;
  uint8_t* pixels;
} h2_g99_image_t;

/** One page: its wheel and its info area, moved together in transitions. */
typedef struct h2_g99_layer {
  lv_obj_t* wheel;
  lv_obj_t* glow;
  lv_obj_t* poster;
  lv_obj_t* slots[H2_G99_WHEEL_SLOTS];
  const void* slot_src[H2_G99_WHEEL_SLOTS];
  lv_obj_t* info;
  lv_obj_t* wifi_rows[H2_G99_WIFI_MAX_NETWORKS];
  lv_obj_t* timeout_rows[H2_G99_SCREEN_TIMEOUT_COUNT];
  lv_obj_t* wifi_controls;
  lv_obj_t* wifi_connect;
  uint64_t wifi_reveal_ms;
  lv_obj_t* slider;
  lv_obj_t* slider_number;
  h2_g99_route_t route;
  uint8_t sel;
  uint32_t info_rev;
  bool built;
} h2_g99_layer_t;

/** Requests raised by LVGL callbacks, applied by the Main loop. */
typedef struct h2_g99_touch_request {
  int action;
  int row;
  int value;
  int text;
  int wifi_focus;
  bool value_end;
  int haptics;
  int timeout;
  int library_menu;
  bool feedback;
} h2_g99_touch_request_t;

enum {
  H2_G99_TEXT_NONE = 0,
  H2_G99_TEXT_SUBMIT,
  H2_G99_TEXT_CANCEL,
};

typedef struct h2_g99_app {
  h2_runtime_t* runtime;
  const h2_g99_config_t* config;
  h2_g99_layout_t layout;
  h2_g99_nav_t nav;
  h2_g99_settings_t settings;
  h2_g99_wifi_t* wifi;
  h2_g99_wifi_snapshot_t wifi_state;
  h2_g99_storage_t* storage;
  h2_g99_storage_snapshot_t storage_state;
  uint64_t storage_query_ms;
  h2_g99_haptics_t haptics;
  bool haptics_available;
  uint64_t last_scan_ms;
  bool scanned;
  h2_g99_lifecycle_t lifecycle;
  h2_g99_observation_t observation;
  bool observation_sent;
  bool display_opened;
  uint64_t now_ms;
  uint64_t last_tick_ms;
  h2_runtime_timestamp_ms_t last_press[H2_G99_BUTTON_COUNT];
  uint8_t battery_percent;
  bool battery_valid;
  uint64_t last_battery_ms;
  bool settings_loaded;
  /* Wall-clock minute shown in the status bar. */
  uint64_t shown_minute;
  char device_id[18];

  /* LVGL. */
  bool lvgl_ready;
  lv_display_t* display;
  uint8_t* render_buffer;
  size_t render_bytes;
  bool suppress_flush;
  h2_pal_result_t flush_result;
  h2_lvgl_touch_t* touch;
  lv_font_t* font_hero;
  lv_font_t* font_title;
  lv_font_t* font_body;
  lv_font_t* font_small;
  lv_font_t* font_big;
  lv_obj_t* screen;
  lv_obj_t* background;
  lv_obj_t* catcher;
  lv_obj_t* brand_icon;
  lv_obj_t* wordmark;
  lv_obj_t* blackout;
  lv_obj_t* keyboard_panel;
  lv_obj_t* keyboard_text;
  lv_obj_t* keyboard;
  lv_obj_t* library_overlay;
  lv_obj_t* library_scrim;
  lv_obj_t* library_drawer;
  bool library_menu_open;
  float library_menu_progress;
  float library_menu_from;
  uint64_t library_menu_ms;
  uint32_t library_menu_duration_ms;
  char keyboard_ssid[H2_PAL_WIFI_SSID_MAX + 1];
  h2_g99_layer_t layers[2];
  uint8_t front;
  bool swap_pending;
  uint32_t info_rev;
  h2_g99_image_t bg;
  h2_g99_background_t bg_spec;
  h2_g99_image_t icons[H2_G99_ICON_COUNT];
  h2_g99_image_t cover;
  h2_g99_image_t library_poster;
  h2_g99_image_t glow;
  h2_g99_image_t brand;
  h2_g99_image_t mark;
  h2_g99_image_t tiny_wifi;
  h2_g99_image_t tiny_lock;
  h2_g99_image_t tiny_check;
  int32_t icon_px;
  int32_t thumb_px;
  int32_t glow_px;
  bool press_active;
  bool press_ignored;
  bool press_wifi;
  bool press_timeout;
  lv_point_t press_start;
  /* Touch reaches LVGL through this filter, see h2_g99_touch.h; its
   * counters at the start of the current press. */
  h2_g99_touch_filter_t touch_filter;
  h2_g99_touch_stats_t press_touch;
  /* A press that started on a turnable wheel and moved past the slop turns
   * it with the finger: drag_offset items past the selection. */
  bool dragging;
  float drag_offset;
  uint64_t drag_last_ms;
  /* Touch diagnostics for the current press: touch reads LVGL made and the
   * longest gap between two of them. */
  uint32_t press_samples;
  uint64_t press_start_ms;
  uint64_t press_last_ms;
  uint32_t press_gap_ms;
  h2_g99_touch_request_t touch_request;
  h2_g99_list_motion_t wifi_motion;
  h2_g99_rect_t wifi_list_rect;
  bool wifi_motion_ready;
  h2_g99_list_motion_t timeout_motion;
  h2_g99_rect_t timeout_list_rect;
  bool timeout_motion_ready;
  bool wifi_connect_pressed;
  bool wifi_connect_dragged;
  lv_point_t wifi_connect_start;
  bool slider_pressed;
  bool slider_dirty;
  bool haptics_pressed;

  /* Frame statistics for config->frame, accumulated until a frame is
   * presented. */
  uint32_t frame_index;
  uint64_t frame_last_us;
  uint64_t frame_draw_us;
  uint64_t frame_present_us;
  uint64_t frame_pixels;
  bool frame_presented;

  /* Lua app. */
  h2_lua_host_t* host;
  h2_lua_job_id_t job;
} h2_g99_app_t;

/* UI (h2_g99_ui.c). */
h2_pal_result_t h2_g99_ui_init(h2_g99_app_t* app);
void h2_g99_ui_deinit(h2_g99_app_t* app);
/** Places every animated element for now_ms and advances the background. */
void h2_g99_ui_frame(h2_g99_app_t* app);
/** Runs LVGL timers after advancing its tick to now_ms. */
void h2_g99_ui_pump(h2_g99_app_t* app);
void h2_g99_ui_set_screen(h2_g99_app_t* app, bool on);
/** Stops LVGL drawing before an app borrows the display. */
void h2_g99_ui_suspend(h2_g99_app_t* app);
/** Redraws the whole screen after the app released the display. */
void h2_g99_ui_resume(h2_g99_app_t* app);
void h2_g99_ui_open_keyboard(h2_g99_app_t* app, const char* ssid);
/** Replaces the text typed into the open keyboard. */
void h2_g99_ui_set_keyboard_text(h2_g99_app_t* app, const char* text);
/** Copies the typed text and closes the keyboard; buffer may be NULL. */
void h2_g99_ui_close_keyboard(h2_g99_app_t* app, char* text, size_t capacity);
void h2_g99_ui_wifi_select(h2_g99_app_t* app, uint8_t selection, bool animate);
void h2_g99_ui_timeout_select(h2_g99_app_t* app, uint8_t selection,
                              bool animate);
void h2_g99_ui_library_menu(h2_g99_app_t* app, bool open);

/* Text (h2_g99_ui.c). */
const char* h2_g99_main_name(uint8_t item);
const char* h2_g99_setting_name(uint8_t item);

/* Lua app (h2_g99_lua.c). */
h2_pal_result_t h2_g99_lua_launch(h2_g99_app_t* app);
/** Returns true while the app keeps running. */
bool h2_g99_lua_poll(h2_g99_app_t* app);
h2_pal_result_t h2_g99_lua_stop(h2_g99_app_t* app);

void h2_g99_log(const h2_g99_app_t* app, const char* format, ...);

/** Monotonic time in microseconds; millisecond resolution when the target
 * has no microsecond clock. */
uint64_t h2_g99_now_us(const h2_g99_app_t* app);

#ifdef __cplusplus
}
#endif

#endif
