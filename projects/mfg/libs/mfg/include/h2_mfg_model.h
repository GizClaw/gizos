#ifndef H2_MFG_MODEL_H
#define H2_MFG_MODEL_H

#include "h2_loader_boot.h"
#include "h2/pal/hal/h2_pal_periph.h"
#include "h2/pal/hal/h2_pal_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_mfg_check {
  H2_MFG_STORAGE,
  H2_MFG_DISPLAY,
  H2_MFG_TOUCH,
  H2_MFG_BUTTONS,
  H2_MFG_IMU,
  H2_MFG_SPEAKER,
  H2_MFG_MIC,
  H2_MFG_WIFI,
  H2_MFG_BLE,
  H2_MFG_LED,
  H2_MFG_MODEM,
  H2_MFG_AEC,
  H2_MFG_MAGNETOMETERS,
  H2_MFG_BATTERY,
  H2_MFG_VIBRATION,
  H2_MFG_CAMERA,
  H2_MFG_BRIGHTNESS,
} h2_mfg_check_t;

typedef struct h2_mfg_step {
  const char* name;
  h2_mfg_check_t check;
  int automatic;
} h2_mfg_step_t;

#define H2_MFG_BUTTON_MAX 8u
#define H2_MFG_MAGNETOMETER_MAX 2u

/** Declarative presentation geometry in display pixels; no board or SDK types.
 * The profile borrows this immutable config for its complete session lifetime.
 * Drawing and touch use these same rectangles. */
typedef struct h2_mfg_ui_rect {
  int x, y, width, height;
} h2_mfg_ui_rect_t;

typedef struct h2_mfg_ui_config {
  uint32_t width, height;
  const char* title;
  uint16_t background, accent, text, passed, failed, muted, disabled;
  h2_mfg_ui_rect_t title_area, heading, progress;
  h2_mfg_ui_rect_t controls[2], retest;
  h2_mfg_ui_rect_t display_pattern, display_note;
  h2_mfg_ui_rect_t brightness_levels, brightness_control, brightness_note;
  h2_mfg_ui_rect_t touch_targets[9];
  uint32_t touch_tolerance;
  h2_mfg_ui_rect_t rows[8];
  h2_mfg_ui_rect_t magnet_boxes[H2_MFG_MAGNETOMETER_MAX];
  h2_mfg_ui_rect_t magnet_labels[H2_MFG_MAGNETOMETER_MAX];
  h2_mfg_ui_rect_t magnet_errors[H2_MFG_MAGNETOMETER_MAX];
  /** Visual slot -> profile magnetometer index, with profile-owned labels. */
  uint32_t magnet_order[H2_MFG_MAGNETOMETER_MAX];
  const char* magnet_names[H2_MFG_MAGNETOMETER_MAX];
  h2_mfg_ui_rect_t vibration, preview;
} h2_mfg_ui_config_t;

typedef struct h2_mfg_profile {
  const char* board;
  uint32_t revision;
  const h2_mfg_step_t* steps;
  uint32_t total;
  /** Explicit unchanged-prefix migration; both zero use normal reset rules. */
  uint32_t append_from_total;
  uint32_t append_from_revision;
  /** 0 uses the shared 70% speaker check; 1..100 is board selected. */
  uint32_t speaker_test_volume;
  h2_pal_periph_id_t buttons[H2_MFG_BUTTON_MAX];
  const char* button_names[H2_MFG_BUTTON_MAX];
  uint32_t button_count;
  /** A physical EN/RESET key requires operator restart/resume acceptance. */
  int has_reset_button;
  /** A dedicated power-controller key requires operator off/on acceptance. */
  int has_power_button;
  /** Nonzero selects a radio-button group instead of single-button reads. */
  h2_pal_periph_id_t button_group;
  h2_pal_periph_id_t imu;
  h2_pal_periph_id_t led;
  h2_pal_periph_id_t magnetometers[H2_MFG_MAGNETOMETER_MAX];
  uint32_t magnetometer_count;
  h2_pal_periph_id_t battery;
  h2_pal_periph_id_t motor;
  /* Signed 1-based native accelerometer axes: X=1, Y=2, Z=3. */
  int screen_x_axis;
  int screen_y_axis;
  /** Disable physical confirm shortcuts; keep ticketed commands and touch. */
  int touch_navigation;
  /** Nine ordered fresh taps rather than the default five-point observation. */
  int sequential_touch_grid;
  /** One fresh down/up per configured button, in profile order. */
  int sequential_buttons;
  /** Processed PCM monitor with manual verdict; zero retains staged AEC. */
  int aec_live_monitor;
  /** Separate manual charging indication and unplugged-operation prompts. */
  int battery_manual_charge;
  /** Observe advertisements instead of requiring a BLE command round trip. */
  int ble_scan_only;
  /** Require connected IPv4 plus manual verdict; no transfer measurement. */
  int wifi_manual_ipv4;
  /** Live baseline response per magnetometer instead of min/max movement. */
  int live_magnets;
  /** Touch-held motor drive instead of the default timed pulses. */
  int hold_vibration;
  /** Optional shared presentation; NULL keeps the existing compact renderer. */
  const h2_mfg_ui_config_t* ui;
  /** Visit the remaining checks after an explicit verdict, retaining failures.
   */
  int sequential_traversal;
  /** Successful peripheral shutdown is required before committing PASS. */
  int cleanup_before_pass;
  /** Start/acquire/convert camera frames outside the controller task. */
  int camera_async;
  /** Cycle and visually confirm all four display patterns. */
  int manual_display_patterns;
  /** Optional test network, borrowed for the session; NULL only observes STA.
   */
  const h2_pal_wifi_sta_config_t* wifi_test_config;
  uint32_t wifi_timeout_ms;
  /** Per-axis invalid compensated value; used only with live_magnets. */
  int32_t magnetic_invalid_mgauss;
  int32_t magnetic_delta_mgauss;
  int32_t accel_delta_mg;
  int32_t gyro_delta_mdps;
  uint32_t scan_window_ms;
  uint32_t monitor_min_frames;
  uint32_t vibration_percent;
  uint32_t preview_width;
  uint32_t preview_height;
} h2_mfg_profile_t;

typedef int (*h2_mfg_save_fn)(void* user,
                              const h2_loader_mfg_summary_t* summary);

/** Single-owner state machine. The session serializes callers. A failed save
 * leaves progress, selection and confirmation ticket unchanged. */
typedef struct h2_mfg_model {
  const h2_mfg_profile_t* profile;
  h2_loader_mfg_summary_t summary;
  uint32_t current;
  uint32_t ticket;
  int ready;
  int error;
  void* save_user;
  h2_mfg_save_fn save;
} h2_mfg_model_t;

int h2_mfg_model_init(h2_mfg_model_t* model, const h2_mfg_profile_t* profile,
                      const h2_loader_mfg_summary_t* summary, uint32_t ticket,
                      h2_mfg_save_fn save, void* save_user);
int h2_mfg_model_complete(const h2_mfg_model_t* model);
/** confirm/fail/skip/retry/reset, bound to the displayed ticket. confirm also
 * requires matching step name, real readiness and a successful check. retry
 * accepts any step name, including after completion. reset uses step="all". */
int h2_mfg_model_action(h2_mfg_model_t* model, const char* action,
                        const char* step, uint32_t ticket);

#ifdef __cplusplus
}
#endif
#endif
