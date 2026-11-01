#ifndef H2_H202_MFG_H
#define H2_H202_MFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @file
 * Private H202 voice-guided MFG, not a shared h2_mfg_* session.
 *
 * Three layers, all portable (PAL IDs only, no SDK or board types):
 * - The controller is a synchronous ten-stage state machine. It never blocks,
 *   owns no task and talks to hardware only through begin/cancel/light.
 * - Pure helpers: progress through h2_loader_mfg_*, charge indication, 1 kHz
 *   Goertzel AEC measurement and 8 kHz prompt playback.
 * - The service owns one controller task plus one worker task per operation
 *   and maps workers onto the Runtime PAL providers.
 */

enum {
  H2_H202_MFG_KEY_POWER = 1u,
  H2_H202_MFG_KEY_RECORD = 2u,
  H2_H202_MFG_KEY_PLUS = 4u,
  H2_H202_MFG_KEY_MINUS = 8u,
  H2_H202_MFG_KEY_MASK = 15u,
};

/** The eleven results, in the order stored through h2_loader_mfg_*. */
typedef enum h2_h202_mfg_item {
  H2_H202_MFG_CHARGER,
  H2_H202_MFG_NFC,
  H2_H202_MFG_WIFI,
  H2_H202_MFG_CELLULAR,
  H2_H202_MFG_LEFT_MIC,
  H2_H202_MFG_RIGHT_MIC,
  H2_H202_MFG_LISTEN,
  H2_H202_MFG_POWER,
  H2_H202_MFG_PLUS,
  H2_H202_MFG_MINUS,
  H2_H202_MFG_AGING,
  H2_H202_MFG_ITEM_COUNT,
} h2_h202_mfg_item_t;

/** Values equal h2_loader_mfg_step_status_t. */
typedef enum h2_h202_mfg_result {
  H2_H202_MFG_UNTESTED = 0,
  H2_H202_MFG_PASSED = 1,
  H2_H202_MFG_SKIPPED = 2,
  H2_H202_MFG_FAILED = 3,
} h2_h202_mfg_result_t;

typedef enum h2_h202_mfg_prompt {
  H2_H202_MFG_PROMPT_NONE = -1,
  H2_H202_MFG_PROMPT_CHARGE_INSERT = 0,
  H2_H202_MFG_PROMPT_CHARGE_REMOVE,
  H2_H202_MFG_PROMPT_CHARGE_PASSED,
  H2_H202_MFG_PROMPT_NFC,
  H2_H202_MFG_PROMPT_NFC_PASSED,
  H2_H202_MFG_PROMPT_WIFI_PASSED,
  H2_H202_MFG_PROMPT_CELLULAR_PASSED,
  H2_H202_MFG_PROMPT_MIC_LEFT,
  H2_H202_MFG_PROMPT_MIC_RIGHT,
  H2_H202_MFG_PROMPT_LISTEN,
  H2_H202_MFG_PROMPT_POWER,
  H2_H202_MFG_PROMPT_PLUS,
  H2_H202_MFG_PROMPT_MINUS,
  H2_H202_MFG_PROMPT_AGING,
  H2_H202_MFG_PROMPT_WAITING,
  H2_H202_MFG_PROMPT_CHARGE_FAILED,
  H2_H202_MFG_PROMPT_NFC_FAILED,
  H2_H202_MFG_PROMPT_WIFI_FAILED,
  H2_H202_MFG_PROMPT_CELLULAR_FAILED,
  H2_H202_MFG_PROMPT_MIC_FAILED,
  H2_H202_MFG_PROMPT_AGING_FAILED,
  H2_H202_MFG_PROMPT_INCOMPLETE,
  H2_H202_MFG_PROMPT_UNSAVED,
  H2_H202_MFG_PROMPT_POWEROFF_FAILED,
  H2_H202_MFG_PROMPT_COUNT,
} h2_h202_mfg_prompt_t;

typedef enum h2_h202_mfg_color {
  H2_H202_MFG_COLOR_OFF,
  H2_H202_MFG_COLOR_AMBER,
  H2_H202_MFG_COLOR_PURPLE,
  H2_H202_MFG_COLOR_CYAN,
  H2_H202_MFG_COLOR_BLUE,
  H2_H202_MFG_COLOR_BLUE_LEFT,
  H2_H202_MFG_COLOR_BLUE_RIGHT,
  H2_H202_MFG_COLOR_GREEN,
  H2_H202_MFG_COLOR_YELLOW,
  H2_H202_MFG_COLOR_RED,
  H2_H202_MFG_COLOR_COUNT,
} h2_h202_mfg_color_t;

/** One worker task per operation; a new begin preempts the running job. */
typedef enum h2_h202_mfg_operation {
  H2_H202_MFG_OP_AUDIO,
  H2_H202_MFG_OP_SENSOR,
  H2_H202_MFG_OP_WIFI,
  H2_H202_MFG_OP_CELLULAR,
  H2_H202_MFG_OP_POWER,
  H2_H202_MFG_OP_COUNT,
} h2_h202_mfg_operation_t;

typedef enum h2_h202_mfg_job {
  H2_H202_MFG_JOB_PROMPT,   /**< AUDIO: play prompts[0..prompt_count). */
  H2_H202_MFG_JOB_MIC,      /**< AUDIO: instruction prompt, then measure. */
  H2_H202_MFG_JOB_AGING,    /**< AUDIO: prompt, tone, dual AEC, rainbow. */
  H2_H202_MFG_JOB_CHARGER,  /**< SENSOR: report USB_INSERT state changes. */
  H2_H202_MFG_JOB_NFC,      /**< SENSOR: wait for any ISO14443A UID. */
  H2_H202_MFG_JOB_CHECK,    /**< WIFI/CELLULAR: one qualification attempt. */
  H2_H202_MFG_JOB_MAINTAIN, /**< WIFI/CELLULAR: restart, keep reconnecting. */
  H2_H202_MFG_JOB_SHUTDOWN, /**< POWER: commit results, then power off. */
} h2_h202_mfg_job_t;

typedef struct h2_h202_mfg_request {
  h2_h202_mfg_operation_t operation;
  uint32_t generation; /**< Nonzero; completions echo it. */
  h2_h202_mfg_job_t job;
  h2_h202_mfg_item_t item;
  h2_h202_mfg_prompt_t prompts[2];
  unsigned prompt_count;
  uint8_t results[H2_H202_MFG_ITEM_COUNT]; /**< SHUTDOWN only. */
} h2_h202_mfg_request_t;

typedef enum h2_h202_mfg_event {
  H2_H202_MFG_EVENT_DONE,        /**< Terminal: the job has released all. */
  H2_H202_MFG_EVENT_CHARGER,     /**< value: 1 inserted, 0 removed. */
  H2_H202_MFG_EVENT_COMMITTED,   /**< SHUTDOWN progress write result. */
  H2_H202_MFG_EVENT_POWER_ERROR, /**< SHUTDOWN power-off returned. */
} h2_h202_mfg_event_t;

/** MIC DONE value: h2_h202_mfg_aec_verdict_t. */
typedef struct h2_h202_mfg_completion {
  h2_h202_mfg_operation_t operation;
  uint32_t generation;
  h2_h202_mfg_event_t event;
  h2_pal_result_t result;
  int32_t value;
} h2_h202_mfg_completion_t;

/** All callbacks run on the controller owner. begin copies the request and
 * returns without waiting. cancel asks the job with that generation to stop;
 * it still reports DONE (aging uses this as its clean stop). light errors are
 * logged only. */
typedef struct h2_h202_mfg_callbacks {
  void* user;
  h2_pal_result_t (*begin)(void* user, const h2_h202_mfg_request_t* request);
  void (*cancel)(void* user, h2_h202_mfg_operation_t operation,
                 uint32_t generation);
  h2_pal_result_t (*light)(void* user, h2_h202_mfg_color_t color);
  /** Optional diagnostics line, borrowed for the call. */
  void (*log)(void* user, const char* line);
} h2_h202_mfg_callbacks_t;

typedef struct h2_h202_mfg_config {
  uint32_t charger_timeout_ms;
  uint32_t nfc_timeout_ms;
  uint32_t wifi_timeout_ms;
  uint32_t cellular_timeout_ms;
  uint32_t power_hold_ms;
  h2_h202_mfg_callbacks_t callbacks;
} h2_h202_mfg_config_t;

typedef enum h2_h202_mfg_phase {
  H2_H202_MFG_PHASE_IDLE,
  H2_H202_MFG_PHASE_ACTIVE,   /**< Current item is running. */
  H2_H202_MFG_PHASE_ANNOUNCE, /**< Success prompt, then the next item. */
  H2_H202_MFG_PHASE_FAILED,   /**< Red; Record retries, skip records SKIPPED. */
  H2_H202_MFG_PHASE_STOPPING, /**< Aging stop requested, awaiting clean DONE. */
  H2_H202_MFG_PHASE_WAITING,  /**< Stage 10. */
  H2_H202_MFG_PHASE_SAVING,
  H2_H202_MFG_PHASE_UNSAVED,
  H2_H202_MFG_PHASE_POWEROFF_FAILED,
} h2_h202_mfg_phase_t;

typedef enum h2_h202_mfg_net_state {
  H2_H202_MFG_NET_IDLE,
  H2_H202_MFG_NET_RUNNING,
  H2_H202_MFG_NET_PASSED,
  H2_H202_MFG_NET_FAILED,
} h2_h202_mfg_net_state_t;

typedef struct h2_h202_mfg_net {
  h2_h202_mfg_net_state_t state;
  uint32_t generation;
  uint64_t deadline_ms;
  h2_pal_result_t error;
} h2_h202_mfg_net_t;

/** Restored one-shot progress; only PASSED entries are honored. */
typedef struct h2_h202_mfg_restore {
  h2_pal_result_t error; /**< Non-OK: results discarded, start at stage 02. */
  bool present;
  uint8_t results[H2_H202_MFG_ITEM_COUNT];
} h2_h202_mfg_restore_t;

/** Controller state. Fields are private to h2_h202_mfg.c; read status. */
typedef struct h2_h202_mfg {
  h2_h202_mfg_config_t config;
  h2_h202_mfg_phase_t phase;
  h2_h202_mfg_item_t item;
  uint8_t results[H2_H202_MFG_ITEM_COUNT];
  uint32_t next_generation;
  uint32_t current[H2_H202_MFG_OP_COUNT];
  h2_h202_mfg_job_t audio_job;
  bool maintaining;
  h2_h202_mfg_net_t net[2];
  uint64_t now_ms;
  uint64_t deadline_ms;
  bool deadline_set;
  unsigned keys;
  unsigned pressed_since_entry;
  bool record_chord;
  bool plus_chord;
  bool power_held;
  uint64_t power_down_ms;
  int charger_initial;
  unsigned charger_edges;
  bool committed;
  h2_pal_result_t error;
  h2_pal_result_t restore_error;
  h2_h202_mfg_color_t color;
  h2_h202_mfg_prompt_t prompt;
} h2_h202_mfg_t;

typedef struct h2_h202_mfg_status {
  h2_h202_mfg_phase_t phase;
  unsigned stage; /**< 1..10 as in the product flow. */
  h2_h202_mfg_item_t item;
  uint8_t results[H2_H202_MFG_ITEM_COUNT];
  h2_h202_mfg_net_state_t wifi;
  h2_h202_mfg_net_state_t cellular;
  h2_pal_result_t error;         /**< Last item or audio failure. */
  h2_pal_result_t restore_error; /**< Progress read/clear failure. */
  h2_h202_mfg_color_t color;     /**< Last requested eye color. */
  h2_h202_mfg_prompt_t prompt;   /**< Last requested prompt. */
  bool committed;
} h2_h202_mfg_status_t;

/** Copies config; starts nothing. Missing begin/cancel/light: INVALID_ARG. */
h2_pal_result_t h2_h202_mfg_init(h2_h202_mfg_t* app,
                                 const h2_h202_mfg_config_t* config);
/** Stage 01: start background networks not PASSED, then resume at the first
 * item not PASSED. keys is the current stable mask; held keys need a new
 * press. restore may be NULL (no record). */
h2_pal_result_t h2_h202_mfg_start(h2_h202_mfg_t* app,
                                  const h2_h202_mfg_restore_t* restore,
                                  uint64_t now_ms, unsigned keys);
/** One owner step: keys first, then completions (old generations ignored),
 * then deadlines. now_ms must not go backwards. */
h2_pal_result_t h2_h202_mfg_update(h2_h202_mfg_t* app, uint64_t now_ms,
                                   unsigned keys,
                                   const h2_h202_mfg_completion_t* completions,
                                   size_t count);
h2_pal_result_t h2_h202_mfg_status(const h2_h202_mfg_t* app,
                                   h2_h202_mfg_status_t* out);
/** Stage of an item (02..09). */
unsigned h2_h202_mfg_stage_of(h2_h202_mfg_item_t item);

/** Revision bound to the eleven results by
 * h2_loader_mfg_ensure_acceptance_revision. Change it with the criteria. */
#define H2_H202_MFG_REVISION 0x20200001u

/** Boot: bind revision, read, then reset the one-shot record before any stage
 * runs. The caller holds the Loader operation mutex. Never fails: errors are
 * reported in out->error with results discarded. */
void h2_h202_mfg_progress_load(const h2_pal_pref_api_t* pref,
                               const h2_pal_mem_api_t* mem,
                               h2_h202_mfg_restore_t* out);
/** Power-hold shutdown only. The caller holds the operation mutex. */
h2_pal_result_t h2_h202_mfg_progress_commit(
    const h2_pal_pref_api_t* pref,
    const uint8_t results[H2_H202_MFG_ITEM_COUNT]);

/** Background white-LED charge indicator, from raw asserted levels. */
typedef struct h2_h202_mfg_charge {
  uint32_t window_ms;       /**< CHRG toggle detection window (2000). */
  unsigned toggles;         /**< Changes within window that mean toggling. */
  uint32_t blink_period_ms; /**< 500 for 2 Hz. */
  bool have_chrg;
  bool chrg;
  uint64_t changes[8];
  unsigned change_count;
} h2_h202_mfg_charge_t;

typedef enum h2_h202_mfg_charge_mode {
  H2_H202_MFG_CHARGE_OFF,
  H2_H202_MFG_CHARGE_ON,
  H2_H202_MFG_CHARGE_BLINK,
} h2_h202_mfg_charge_mode_t;

void h2_h202_mfg_charge_init(h2_h202_mfg_charge_t* charge, uint32_t window_ms,
                             unsigned toggles, uint32_t blink_period_ms);
/** Feed one sample; returns the mode, *led the white LED level now. */
h2_h202_mfg_charge_mode_t h2_h202_mfg_charge_update(
    h2_h202_mfg_charge_t* charge, uint64_t now_ms, bool usb_insert,
    bool charging, bool done, bool* led);

/** Stage 05 measurement parameters; provisional values live in profile.c. */
typedef struct h2_h202_mfg_aec_config {
  uint32_t sample_rate_hz;
  uint32_t tone_hz;
  uint32_t convergence_ms;
  uint32_t measure_ms;
  uint32_t block_ms;            /**< At least 100. */
  int32_t min_tone_dbfs_x10;    /**< -400: reference and raw lane. */
  int32_t min_reduction_db_x10; /**< 100: raw minus processed. */
} h2_h202_mfg_aec_config_t;

typedef enum h2_h202_mfg_aec_verdict {
  H2_H202_MFG_AEC_PASS = 0,
  H2_H202_MFG_AEC_NO_SIGNAL = 1,     /**< No complete lane frame or all zero. */
  H2_H202_MFG_AEC_REFERENCE_LOW = 2, /**< Loopback or its ADC lane broken. */
  H2_H202_MFG_AEC_RAW_LOW = 3,       /**< Silent speaker or microphone. */
  H2_H202_MFG_AEC_REDUCTION_LOW = 4,
  H2_H202_MFG_AEC_INCOMPLETE = 5, /**< Window not finished yet. */
} h2_h202_mfg_aec_verdict_t;

enum { H2_H202_MFG_AEC_MAX_BLOCKS = 128 };

typedef struct h2_h202_mfg_aec_meter {
  h2_h202_mfg_aec_config_t config;
  uint8_t lane;
  uint32_t block_samples;
  uint64_t lane_samples;
  uint32_t lane_frames;
  bool nonzero;
  float coefficient;
  float raw[2], reference[2], processed[2]; /**< Goertzel s1/s2. */
  uint32_t in_block;
  unsigned blocks;
  int16_t raw_db_x10[H2_H202_MFG_AEC_MAX_BLOCKS];
  int16_t reference_db_x10[H2_H202_MFG_AEC_MAX_BLOCKS];
  int16_t reduction_db_x10[H2_H202_MFG_AEC_MAX_BLOCKS];
} h2_h202_mfg_aec_meter_t;

typedef struct h2_h202_mfg_aec_result {
  h2_h202_mfg_aec_verdict_t verdict;
  unsigned blocks;
  int32_t raw_dbfs_x10;       /**< Median tone level on the raw lane. */
  int32_t reference_dbfs_x10; /**< Median tone level on the reference. */
  int32_t reduction_db_x10;   /**< Median per-block echo reduction. */
} h2_h202_mfg_aec_result_t;

h2_pal_result_t h2_h202_mfg_aec_meter_init(
    h2_h202_mfg_aec_meter_t* meter, const h2_h202_mfg_aec_config_t* config,
    uint8_t lane);
/** Observer on_capture body: accepts only this lane's well-formed frames. */
void h2_h202_mfg_aec_meter_capture(h2_h202_mfg_aec_meter_t* meter,
                                   const h2_audio_aec_frame_t* frame);
bool h2_h202_mfg_aec_meter_done(const h2_h202_mfg_aec_meter_t* meter);
h2_h202_mfg_aec_verdict_t h2_h202_mfg_aec_meter_result(
    const h2_h202_mfg_aec_meter_t* meter, h2_h202_mfg_aec_result_t* out);

/** Embedded raw 8 kHz mono PCM16LE prompt. */
typedef struct h2_h202_mfg_pcm {
  const unsigned char* data;
  const size_t* size;
} h2_h202_mfg_pcm_t;

/** 8 kHz to 16 kHz: two outputs per input, linear interpolation from the
 * previous sample (*state, start at 0). out must hold 2*count samples. */
void h2_h202_mfg_upsample(const int16_t* in, size_t count, int16_t* out,
                          int16_t* state);
/** Write a whole prompt as 320-sample 16 kHz frames, zero-padding the final
 * partial frame, then drain. canceled may be NULL. Odd byte counts are
 * INVALID_ARG; a canceled play returns CLOSED; write errors are returned. */
h2_pal_result_t h2_h202_mfg_play_pcm8k(h2_pal_audio_track_t* track,
                                       const unsigned char* pcm, size_t bytes,
                                       bool (*canceled)(void* user),
                                       void* user);

typedef struct h2_h202_mfg_profile {
  h2_h202_mfg_config_t controller; /**< callbacks are set by the service. */
  h2_pal_periph_id_t keys[4];      /**< Power, Record, +, -. */
  h2_pal_periph_id_t usb_insert;
  h2_pal_periph_id_t charging;
  h2_pal_periph_id_t charge_done;
  h2_pal_periph_id_t white;
  h2_pal_periph_id_t nfc;
  h2_pal_led_id_t eyes;
  uint16_t eye_count;
  uint32_t left_eye_mask; /**< Pixels lit for the left microphone. */
  h2_pal_led_color_t colors[H2_H202_MFG_COLOR_COUNT];
  uint32_t tick_ms;
  uint32_t key_debounce_ms;
  uint32_t charger_debounce_ms;
  uint32_t charge_window_ms;
  unsigned charge_toggles;
  uint32_t charge_blink_period_ms;
  uint32_t sample_rate_hz;
  uint16_t frame_samples;
  uint32_t volume_percent;
  uint32_t tone_hz;
  int16_t tone_amplitude;
  uint16_t monitor_gain_q15; /**< AEC output mixed into the tone. */
  h2_h202_mfg_aec_config_t aec;
  uint32_t aging_frame_ms; /**< Rainbow update interval. */
  const char* wifi_ssid;
  const char* wifi_password;
  const char* ntp_host;
  int32_t min_rssi_dbm;
  h2_h202_mfg_pcm_t prompts[H2_H202_MFG_PROMPT_COUNT];
} h2_h202_mfg_profile_t;

typedef struct h2_h202_mfg_service h2_h202_mfg_service_t;

typedef struct h2_h202_mfg_service_config {
  h2_runtime_t* runtime;
  const h2_h202_mfg_profile_t* profile;
  /** From h2_bk_h2loader_app_commands_get_config; Stage-sensitive work,
   * progress and power-off are serialized with it. */
  const h2_pal_sync_api_t* operation_sync;
  h2_pal_mutex_t* operation_mutex;
  /** Discard queued Runtime events so producers never see a full queue. */
  bool drain_runtime_events;
} h2_h202_mfg_service_config_t;

/** Loads one-shot progress on the calling task, then starts the workers and
 * the controller. Runtime, profile and mutex stay borrowed until stop. */
h2_pal_result_t h2_h202_mfg_service_start(
    const h2_h202_mfg_service_config_t* config, h2_h202_mfg_service_t** out);
/** Copy controller status. */
h2_pal_result_t h2_h202_mfg_service_status(h2_h202_mfg_service_t* service,
                                           h2_h202_mfg_status_t* out);
/** Stop and join every task without saving; frees on success. Host tests
 * only: on the device a management reboot ends MFG. */
h2_pal_result_t h2_h202_mfg_service_stop(h2_h202_mfg_service_t** service);

#ifdef __cplusplus
}
#endif
#endif
