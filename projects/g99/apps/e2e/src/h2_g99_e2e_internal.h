#ifndef H2_G99_E2E_INTERNAL_H
#define H2_G99_E2E_INTERNAL_H

#include "os/h2_pal_sync.h"
#include "os/h2_pal_task.h"
#include "h2_g99_e2e.h"
#include "h2_runtime_test.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_G99_E2E_SCHEMA 1u
#define H2_G99_E2E_DETAIL_MAX 191u
#define H2_G99_E2E_CASE_ID_MAX 63u

/* Budgets shared by every case. */
#define H2_G99_E2E_START_MS 10000u
#define H2_G99_E2E_REST_MS 10000u
#define H2_G99_E2E_AFTER_CASE_MS 5000u
#define H2_G99_E2E_NAV_MS 3000u
#define H2_G99_E2E_KEY_HOLD_MS 40u
#define H2_G99_E2E_POLL_MS 10u
/* Touch: how long a step waits for Main to read a finger state, and how
 * often a moving finger changes position. */
#define H2_G99_E2E_TOUCH_READ_MS 1000u
#define H2_G99_E2E_TOUCH_MOVE_MS 10u
/* Main joins a contact that returns within 50 ms of a release to the press
 * before it, which bridges a controller between two reports. A lifted
 * finger stays up until Main read it up this long after the lift, so the
 * next gesture is a new press. */
#define H2_G99_E2E_LIFT_MS 80u
/* The controller models publish a report this often; slower than Main reads
 * touch, so reads between two reports happen. */
#define H2_G99_E2E_REPORT_MS 25u
/* Frame budgets, enforced with H2_G99_E2E_CAP_FRAME_BUDGET: the longest gap
 * between two touch reads, and the frames a wheel rotation must show. */
#define H2_G99_E2E_TOUCH_GAP_BUDGET_MS 50u
#define H2_G99_E2E_ROTATE_MIN_FRAMES 5u

typedef enum h2_g99_e2e_status {
  H2_G99_E2E_PASS = 0,
  H2_G99_E2E_FAIL,
  H2_G99_E2E_ERROR,
  H2_G99_E2E_UNSUPPORTED,
} h2_g99_e2e_status_t;

typedef struct h2_g99_e2e h2_g99_e2e_t;

/* How the scripted finger reports itself, after the touch sources Main
 * meets. Each case starts with CLEAN. */
typedef enum h2_g99_e2e_touch_model {
  /* Every change at the next read, like the desktop and Web sources. */
  H2_G99_E2E_TOUCH_CLEAN = 0,
  /* Mosaico CST9220 with driver 0.1.1: one report every
   * H2_G99_E2E_REPORT_MS; a read between two reports finds an empty frame,
   * which the board reports as a release, and the next report presses
   * again. */
  H2_G99_E2E_TOUCH_EMPTY_FRAMES,
  /* Mosaico CST9220 with driver 0.1.2: a read between two reports fails. */
  H2_G99_E2E_TOUCH_FAILED_READS,
} h2_g99_e2e_touch_model_t;

/* Scripted touch source Main reads instead of Runtime touch. While a case
 * scripts it, it reports the finger the way a touch controller reports a
 * real one: Main sees only the positions it happens to read. Otherwise it
 * passes Runtime touch through. Fields below `api` are guarded by the
 * suite mutex. */
typedef struct h2_g99_e2e_finger {
  h2_g99_e2e_t* e2e;
  const h2_pal_touch_api_t* real;
  h2_pal_touch_api_t api;
  bool scripted;
  bool down;
  int32_t x;
  int32_t y;
  /* Bumped on every scripted change; read_seq is its value at Main's
   * latest read. */
  uint32_t state_seq;
  uint32_t read_seq;
  bool reported_down;
  int32_t reported_x;
  int32_t reported_y;
  h2_g99_e2e_touch_model_t model;
  /* Time of the latest report a controller model published. */
  uint64_t report_ms;
  /* A pointer over the screen without contact, like a mouse moving with no
   * button pressed; the Web platform reports it as moves. */
  bool hovering;
  int32_t hover_x;
  int32_t hover_y;
  bool hover_reported;
  int32_t hover_reported_x;
  int32_t hover_reported_y;
  /* Reads while scripted and the longest gap between two of them. */
  uint64_t begun_ms;
  uint32_t reads;
  uint64_t last_read_ms;
  uint32_t max_gap_ms;
} h2_g99_e2e_finger_t;

/* Main frame statistics since the last reset (h2_g99_frame_stats_t). */
typedef struct h2_g99_e2e_frames {
  uint32_t frames;
  uint64_t interval_us;
  uint32_t max_interval_us;
  uint64_t work_us;
  uint32_t max_work_us;
  uint32_t max_draw_us;
  uint32_t max_present_us;
  uint64_t pixels;
} h2_g99_e2e_frames_t;

typedef struct h2_g99_e2e_touch_stats {
  uint32_t reads;
  uint32_t max_gap_ms;
} h2_g99_e2e_touch_stats_t;

typedef struct h2_g99_e2e_case {
  const char* name;
  uint32_t required_capabilities;
  /* OK passes; otherwise the case set its detail (and error for ERROR). */
  h2_pal_result_t (*run)(h2_g99_e2e_t* e2e);
} h2_g99_e2e_case_t;

typedef struct h2_g99_e2e_group {
  const char* name;
  const h2_g99_e2e_case_t* cases;
  size_t count;
} h2_g99_e2e_group_t;

struct h2_g99_e2e {
  const h2_g99_e2e_target_t* target;
  h2_runtime_t* runtime;
  h2_runtime_test_control_t* control;
  h2_g99_config_t main_config;
  /* Guarded by mutex: written by the Main task, read by the suite. */
  h2_pal_mutex_t* mutex;
  h2_g99_observation_t observation;
  uint64_t observed_ms;
  bool observed;
  bool main_exited;
  h2_pal_result_t main_result;
  bool stop_main;
  h2_g99_e2e_frames_t frames;
  h2_g99_e2e_finger_t finger;
  /* Main task, owned by the suite task. */
  h2_pal_task_t* main_task;
  h2_runtime_timestamp_ms_t last_pressed_at;
  /* Current case. */
  char case_id[H2_G99_E2E_CASE_ID_MAX + 1u];
  char detail[H2_G99_E2E_DETAIL_MAX + 1u];
  bool error;
  /* Run. */
  uint64_t run_started_ms;
  h2_g99_e2e_summary_t summary;
  char first_failure[H2_G99_E2E_CASE_ID_MAX + 1u];
  h2_pal_result_t first_failure_rc;
};

/* Registry (h2_g99_e2e_registry.c), in run order. */
extern const h2_g99_e2e_group_t* const h2_g99_e2e_groups[];
extern const size_t h2_g99_e2e_group_count;
extern const h2_g99_e2e_group_t h2_g99_e2e_startup_group;
extern const h2_g99_e2e_group_t h2_g99_e2e_navigation_group;
extern const h2_g99_e2e_group_t h2_g99_e2e_keys_group;
extern const h2_g99_e2e_group_t h2_g99_e2e_layout_group;
extern const h2_g99_e2e_group_t h2_g99_e2e_settings_group;
extern const h2_g99_e2e_group_t h2_g99_e2e_touch_group;
extern const h2_g99_e2e_group_t h2_g99_e2e_perf_group;
extern const h2_g99_e2e_group_t h2_g99_e2e_wifi_group;

/* Selection (h2_g99_e2e_select.c). */
bool h2_g99_e2e_selected(const char* select, const char* group,
                         const char* name);

/* Session (h2_g99_e2e_session.c). */
uint64_t h2_g99_e2e_now_ms(const h2_g99_e2e_t* e2e);
void h2_g99_e2e_sleep(h2_g99_e2e_t* e2e, uint32_t ms);
h2_pal_result_t h2_g99_e2e_session_open(h2_g99_e2e_t* e2e,
                                        const h2_g99_e2e_target_t* target);
void h2_g99_e2e_session_close(h2_g99_e2e_t* e2e);
/* Starts Main on its own task; observation is reset first. */
h2_pal_result_t h2_g99_e2e_main_start(h2_g99_e2e_t* e2e);
/* Asks Main to stop and joins it; returns Main's result. */
h2_pal_result_t h2_g99_e2e_main_stop(h2_g99_e2e_t* e2e);
h2_pal_result_t h2_g99_e2e_main_restart(h2_g99_e2e_t* e2e);
bool h2_g99_e2e_main_running(h2_g99_e2e_t* e2e);
/* Copies the latest observation; false before Main published one. */
bool h2_g99_e2e_observe(h2_g99_e2e_t* e2e, h2_g99_observation_t* out,
                        uint64_t* out_observed_ms);
/* Presses and releases one logical key through Runtime Test Control. */
h2_pal_result_t h2_g99_e2e_press(h2_g99_e2e_t* e2e,
                                 h2_runtime_component_id_t component);
/* Posts H2_G99_EVENT_TEXT_INPUT. */
h2_pal_result_t h2_g99_e2e_type(h2_g99_e2e_t* e2e, const char* text);
bool h2_g99_e2e_aborted(h2_g99_e2e_t* e2e);
void h2_g99_e2e_lock(h2_g99_e2e_t* e2e);
void h2_g99_e2e_unlock(h2_g99_e2e_t* e2e);
/* Restarts and copies Main frame statistics. */
void h2_g99_e2e_frames_reset(h2_g99_e2e_t* e2e);
void h2_g99_e2e_frames_get(h2_g99_e2e_t* e2e, h2_g99_e2e_frames_t* out);

/* Touch (h2_g99_e2e_touch.c), in Main screen pixels. */
/* Prepares the scripted finger over `real` (may be NULL). */
void h2_g99_e2e_finger_init(h2_g99_e2e_t* e2e, const h2_pal_touch_api_t* real);
/* Drags from (x0, y0) to (x1, y1) over duration_ms and lifts the finger.
 * With `sampled` the finger waits on the touch-down and on the end point
 * until Main read them, like a slow, deliberate drag; without it the
 * finger moves on the clock alone, like a quick flick. */
h2_pal_result_t h2_g99_e2e_drag(h2_g99_e2e_t* e2e, int32_t x0, int32_t y0,
                                int32_t x1, int32_t y1, uint32_t duration_ms,
                                bool sampled, h2_g99_e2e_touch_stats_t* out);
/* Presses (x, y) until Main read it and lifts the finger. */
h2_pal_result_t h2_g99_e2e_tap(h2_g99_e2e_t* e2e, int32_t x, int32_t y);
/* Holds the finger: down at (x, y), moved, and lifted; each step waits until
 * Main read it. */
h2_pal_result_t h2_g99_e2e_touch_down(h2_g99_e2e_t* e2e, int32_t x, int32_t y);
h2_pal_result_t h2_g99_e2e_touch_move(h2_g99_e2e_t* e2e, int32_t x, int32_t y);
h2_pal_result_t h2_g99_e2e_touch_up(h2_g99_e2e_t* e2e);
/* Moves a pointer without contact from (x0, y0) to (x1, y1) over
 * duration_ms, like a mouse with no button pressed, and waits until Main
 * read its end point. */
h2_pal_result_t h2_g99_e2e_hover(h2_g99_e2e_t* e2e, int32_t x0, int32_t y0,
                                 int32_t x1, int32_t y1, uint32_t duration_ms);
/* Selects how the finger reports itself from the next gesture on. */
void h2_g99_e2e_touch_model(h2_g99_e2e_t* e2e, h2_g99_e2e_touch_model_t model);
/* Counts Main's touch reads with the finger up for duration_ms. */
h2_pal_result_t h2_g99_e2e_touch_probe(h2_g99_e2e_t* e2e, uint32_t duration_ms,
                                       h2_g99_e2e_touch_stats_t* out);

/* Steps (h2_g99_e2e_steps.c). Every step sets the case detail on failure. */
typedef bool (*h2_g99_e2e_pred_t)(const h2_g99_observation_t* o,
                                  const void* arg);
#define H2_G99_E2E_ANY (-1)
/* -1 fields match anything. */
typedef struct h2_g99_e2e_expect {
  int lifecycle;
  int route;
  int main_selection;
  int sub_selection;
  int transition;
  int input_ready;
  int screen_on;
  int editing;
  int text_entry;
} h2_g99_e2e_expect_t;
/* Every field H2_G99_E2E_ANY. */
h2_g99_e2e_expect_t h2_g99_e2e_any(void);
/* MAIN, RUNNING, input ready, screen on, nothing edited, on `selection`. */
h2_g99_e2e_expect_t h2_g99_e2e_main_ready(int selection);
/* `route` settled on `sub_selection` with input ready and screen on. */
h2_g99_e2e_expect_t h2_g99_e2e_page_ready(h2_g99_route_t route,
                                          int sub_selection);
bool h2_g99_e2e_matches(const h2_g99_observation_t* o,
                        const h2_g99_e2e_expect_t* expect);
void h2_g99_e2e_fail(h2_g99_e2e_t* e2e, const char* format, ...);
void h2_g99_e2e_errorf(h2_g99_e2e_t* e2e, h2_pal_result_t rc, const char* what);
/* Waits up to timeout_ms for `expect` (and `pred` when not NULL). */
h2_pal_result_t h2_g99_e2e_wait(h2_g99_e2e_t* e2e,
                                const h2_g99_e2e_expect_t* expect,
                                h2_g99_e2e_pred_t pred, const void* arg,
                                uint32_t timeout_ms, const char* what,
                                h2_g99_observation_t* out);
/* Requires `expect` to hold on every sample for duration_ms. */
h2_pal_result_t h2_g99_e2e_stays(h2_g99_e2e_t* e2e,
                                 const h2_g99_e2e_expect_t* expect,
                                 uint32_t duration_ms, const char* what);
/* Presses `component`, then waits for `expect` within timeout_ms. */
h2_pal_result_t h2_g99_e2e_press_until(h2_g99_e2e_t* e2e,
                                       h2_runtime_component_id_t component,
                                       const h2_g99_e2e_expect_t* expect,
                                       uint32_t timeout_ms, const char* what);
/* Rotates the main wheel to `item` from a settled main menu. */
h2_pal_result_t h2_g99_e2e_main_select(h2_g99_e2e_t* e2e, uint8_t item);
/* From rest, enters Settings and rotates to `item`. */
h2_pal_result_t h2_g99_e2e_open_setting(h2_g99_e2e_t* e2e, uint8_t item);
/* Rest: MAIN(Library) settled, Main running, nothing edited, screen on. */
bool h2_g99_e2e_at_rest(const h2_g99_observation_t* o);
/* Uses public keys (and a Main restart if Main exited) to reach rest. */
h2_pal_result_t h2_g99_e2e_recover(h2_g99_e2e_t* e2e, uint32_t timeout_ms);
/* Waits for rest without sending input. */
h2_pal_result_t h2_g99_e2e_wait_rest(h2_g99_e2e_t* e2e, uint32_t timeout_ms,
                                     const char* what);

/* Report (h2_g99_e2e_report.c). */
void h2_g99_e2e_record(h2_g99_e2e_t* e2e, const char* format, ...);
/* One H2_E2E_METRIC line for the current case; `fields` is "key=value ...". */
void h2_g99_e2e_metric(h2_g99_e2e_t* e2e, const char* format, ...);
void h2_g99_e2e_report_start(h2_g99_e2e_t* e2e);
void h2_g99_e2e_report_case(h2_g99_e2e_t* e2e, h2_g99_e2e_status_t status,
                            h2_pal_result_t rc, uint64_t duration_ms);
void h2_g99_e2e_report_summary(h2_g99_e2e_t* e2e);

#ifdef __cplusplus
}
#endif

#endif
