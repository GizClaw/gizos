#ifndef H2_G99_E2E_H
#define H2_G99_E2E_H

/**
 * @file h2_g99_e2e.h
 * @brief G99 product E2E suite: launcher contract and run entry.
 *
 * The suite runs the production G99 Main through h2_g99_run(), injects the
 * logical keys through Runtime Test Control, text through the public
 * H2_G99_EVENT_TEXT_INPUT event and touch through a scripted
 * h2_g99_config_t.touch source, and asserts on h2_g99_observation_t and the
 * h2_g99_frame_stats_t Main reports.
 */

#include "h2_g99.h"
#include "h2_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Target capabilities. A case needing one the target lacks is UNSUPPORTED. */
enum {
  /** A deterministic or real Wi-Fi station that can see and join the public
   * test network H2_G99_E2E_WIFI_SSID / H2_G99_E2E_WIFI_PASSWORD, with a
   * Runtime saved-network store. */
  H2_G99_E2E_CAP_WIFI_TEST_NETWORK = 1u << 0,
  /** The target holds the G99 frame budgets: touch read at least every
   * 50 ms and a wheel rotation shown in at least five frames. Without it
   * the perf cases only report their numbers, and cases that depend on
   * real-time touch timing are UNSUPPORTED. */
  H2_G99_E2E_CAP_FRAME_BUDGET = 1u << 1,
};

#define H2_G99_E2E_WIFI_SSID "HAIVIVI-MFG"
#define H2_G99_E2E_WIFI_PASSWORD "!haivivi"

/** Preference namespace E2E runs give Main instead of its production one. */
#define H2_G99_E2E_SETTINGS_NAMESPACE "g99_e2e"

/** Longest selection string, in bytes. */
#define H2_G99_E2E_SELECT_MAX 127u

typedef struct h2_g99_e2e_target {
  /** Started Runtime with input running and the display open; borrowed. */
  h2_runtime_t* runtime;
  /** Report fields, for example "desktop-mosaico" and "desktop". */
  const char* board;
  const char* target;
  /** Display size the target was built for; the layout case checks it. */
  int32_t display_width;
  int32_t display_height;
  /** H2_G99_E2E_CAP_* bits. */
  uint32_t capabilities;
  /** Comma-separated selection; NULL or empty runs every case. */
  const char* select;
  /** Optional mirror of every Main observation (for example a Web page). */
  h2_g99_observe_fn observe;
  void* observe_user;
  /** Optional abort request polled between and inside cases. */
  int (*should_abort)(void* user);
  void* abort_user;
  /** Optional Main render-buffer allocator, borrowed for the entire suite run.
   */
  const h2_pal_mem_api_t* render_mem;
} h2_g99_e2e_target_t;

typedef struct h2_g99_e2e_summary {
  size_t selected;
  size_t passed;
  size_t failed;
  size_t errors;
  size_t unsupported;
  /** Suite setup: selection, fixtures, Test Control, first Main start. */
  h2_pal_result_t setup_rc;
  /** Main stop and Test Control close. */
  h2_pal_result_t cleanup_rc;
  /** Removal of the E2E preference namespace. */
  h2_pal_result_t fixture_restore_rc;
  /** First report write failure. */
  h2_pal_result_t output_rc;
  /** True when every selected case reached a terminal status. */
  bool complete;
} h2_g99_e2e_summary_t;

/**
 * Runs the selected cases and prints H2_E2E_* lines on stdout. Returns OK when
 * the run completed, even with failing cases; the exit code tells the result.
 * Blocks; call from a task that may wait. Main never runs after return.
 */
h2_pal_result_t h2_g99_e2e_run(const h2_g99_e2e_target_t* target,
                               h2_g99_e2e_summary_t* out_summary);

/** 0 when complete without FAIL or ERROR, 1 for failing cases, 2 when the
 * run could not complete. */
int h2_g99_e2e_exit_code(const h2_g99_e2e_summary_t* summary);

/** True when `select` fits H2_G99_E2E_SELECT_MAX and every term matches at
 * least one case; NULL or empty is valid and selects every case. Writes the
 * first unknown term to out_term. */
bool h2_g99_e2e_select_valid(const char* select, char* out_term,
                             size_t out_term_size);

#ifdef __cplusplus
}
#endif

#endif
