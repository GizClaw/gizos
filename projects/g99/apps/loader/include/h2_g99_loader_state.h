#ifndef H2_G99_LOADER_STATE_H
#define H2_G99_LOADER_STATE_H

/* What the GizPocket Loader screen shows, driven by the H2Loader callbacks.
 * Pure C: no LVGL, Runtime or SDK handles. */

#include "h2_loader_package.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_g99_loader_screen {
  /** The brand alone: power-on and launching an installed app. */
  H2_G99_LOADER_SCREEN_BRAND = 0,
  /** Command mode, waiting for a package. */
  H2_G99_LOADER_SCREEN_READY,
  /** An install started; no phase has reported yet. */
  H2_G99_LOADER_SCREEN_INSTALLING,
  H2_G99_LOADER_SCREEN_PROGRESS,
  H2_G99_LOADER_SCREEN_FAILED,
  /** Launching the app this boot just installed. */
  H2_G99_LOADER_SCREEN_STARTING,
} h2_g99_loader_screen_t;

typedef struct h2_g99_loader_state {
  h2_g99_loader_screen_t screen;
  /** Last reported install phase; zero before the first report. */
  h2_loader_install_phase_t phase;
  /** Overall install progress; it never moves backwards. */
  uint32_t percent;
  /** True once an install began in this boot. */
  bool installed;
  /** True after a launch began; a failure then names the launch. */
  bool launching;
  int code;
} h2_g99_loader_state_t;

#define H2_G99_LOADER_STATUS_MAX 64u

/** The drawable result of a state. */
typedef struct h2_g99_loader_view {
  bool status_visible;
  /** Failure tone for the text and the bar. */
  bool error;
  char status[H2_G99_LOADER_STATUS_MAX];
  bool bar_visible;
  uint32_t bar_percent;
} h2_g99_loader_view_t;

void h2_g99_loader_state_init(h2_g99_loader_state_t* state);
void h2_g99_loader_state_ready(h2_g99_loader_state_t* state);
void h2_g99_loader_state_installing(h2_g99_loader_state_t* state);
void h2_g99_loader_state_progress(h2_g99_loader_state_t* state,
                                  h2_loader_install_phase_t phase,
                                  uint64_t completed, uint64_t total);
void h2_g99_loader_state_failed(h2_g99_loader_state_t* state, int code);
/** After an install this shows Starting; otherwise the brand stays alone. */
void h2_g99_loader_state_launching(h2_g99_loader_state_t* state);

void h2_g99_loader_state_view(const h2_g99_loader_state_t* state,
                              h2_g99_loader_view_t* out);

/** Overall percent of `completed` of `total` within `phase`: validation
 * 0-5, image 5-80, data 80-85, PIXA 85-90, verification 90-99, done 100. */
uint32_t h2_g99_loader_phase_percent(h2_loader_install_phase_t phase,
                                     uint64_t completed, uint64_t total);

/** Status name of a phase, for example "Writing image". */
const char* h2_g99_loader_phase_name(h2_loader_install_phase_t phase);

#ifdef __cplusplus
}
#endif

#endif
