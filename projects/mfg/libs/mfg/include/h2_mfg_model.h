#ifndef H2_MFG_MODEL_H
#define H2_MFG_MODEL_H

#include "h2_loader_boot.h"
#include "h2/pal/hal/h2_pal_periph.h"

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
} h2_mfg_check_t;

typedef struct h2_mfg_step {
  const char* name;
  h2_mfg_check_t check;
  int automatic;
} h2_mfg_step_t;

#define H2_MFG_BUTTON_MAX 8u
#define H2_MFG_MAGNETOMETER_MAX 2u

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
