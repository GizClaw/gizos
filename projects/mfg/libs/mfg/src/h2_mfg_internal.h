#ifndef H2_MFG_INTERNAL_H
#define H2_MFG_INTERNAL_H
#include "h2_mfg.h"

typedef struct h2_mfg_aec h2_mfg_aec_t;

struct h2_mfg {
  h2_runtime_t* rt;
  h2_loader_t* loader;
  const h2_mfg_profile_t* profile;
  h2_pal_mutex_t* mutex;
  h2_pal_semaphore_t* finished;
  int running;
  int stopping;
  int initialized;
  int cleanup_result;
  h2_mfg_model_t model;
  uint32_t hardware_ticket;
  uint64_t started_ms;
  uint64_t last_draw_ms;
  uint64_t last_log_ms;
  char detail[160];
  uint16_t* frame;
  h2_display_info_t display;
  int display_open;
  int touch_open;
  int touch_x;
  int touch_y;
  int touch_pressed;
  unsigned touch_action;
  unsigned touch_hits;
  unsigned touch_releases;
  unsigned touch_downs;
  int pressed[H2_MFG_BUTTON_MAX];
  int armed[H2_MFG_BUTTON_MAX];
  unsigned downs[H2_MFG_BUTTON_MAX];
  unsigned ups[H2_MFG_BUTTON_MAX];
  h2_pal_imu_reading_t imu;
  int32_t imu_min[2];
  int32_t imu_max[2];
  unsigned samples;
  h2_audio_info_t audio_info;
  h2_pal_audio_track_t* track;
  int speaker_started;
  int mic_started;
  uint32_t saved_volume;
  int volume_saved;
  void* pcm;
  size_t pcm_bytes;
  unsigned audio_frames;
  uint64_t audio_samples;
  uint32_t phase;
  unsigned mic_peak;
  unsigned mic_level;
  uint64_t mic_level_ms;
  uint64_t mic_energy;
  h2_mfg_aec_t* aec;
  int ble_probed;
  int led_started;
  h2_pal_imu_reading_t magnetometers[H2_MFG_MAGNETOMETER_MAX];
  int32_t mag_min[H2_MFG_MAGNETOMETER_MAX][3];
  int32_t mag_max[H2_MFG_MAGNETOMETER_MAX][3];
  h2_pal_battery_reading_t battery;
  int motor_started;
  uint16_t motor_duty;
  unsigned motor_pulses;
  int camera_started;
  int camera_borrowed;
  h2_pal_camera_frame_t camera_frame;
  uint16_t* camera_preview;
  uint64_t camera_last_ms;
};

enum { H2_MFG_PREVIEW_WIDTH = 160, H2_MFG_PREVIEW_HEIGHT = 90 };
int h2_mfg_peripheral_enter(h2_mfg_t* m);
int h2_mfg_peripheral_tick(h2_mfg_t* m, uint64_t now);
int h2_mfg_peripheral_leave(h2_mfg_t* m);
int h2_mfg_hardware_enter(h2_mfg_t* m, uint64_t now);
int h2_mfg_hardware_tick(h2_mfg_t* m, uint64_t now, const char** action);
int h2_mfg_hardware_leave(h2_mfg_t* m);
int h2_mfg_hardware_close(h2_mfg_t* m);
int h2_mfg_audio_stop(h2_mfg_t* m);
int h2_mfg_audio_tick(h2_mfg_t* m, uint64_t now, int mic);
int h2_mfg_aec_enter(h2_mfg_t* m);
int h2_mfg_aec_tick(h2_mfg_t* m);
int h2_mfg_aec_close(h2_mfg_t* m);
const char* h2_mfg_aec_phase(const h2_mfg_t* m);
/** -1 means no pre/post diagnostics; operator echo/voice acceptance applies. */
int h2_mfg_aec_echo_db(const h2_mfg_t* m);
int h2_mfg_aec_mic_frame(h2_mfg_t* m, const h2_audio_frame_t* frame);
int h2_mfg_ui_draw(h2_mfg_t* m, uint64_t now);
void h2_mfg_touch_target(const h2_mfg_t* m, unsigned i, int* x, int* y);
#endif
