#ifndef H2_G99_LOADER_UI_H
#define H2_G99_LOADER_UI_H

/**
 * @file h2_g99_loader_ui.h
 * @brief GizPocket Loader screen on Runtime Display.
 *
 * Pure black with the GizPocket brand group centred exactly as Main's boot
 * first frame places it, a status line at the bottom left and a full-width
 * progress bar flush with the bottom edge. Calls are serialized internally,
 * so any H2Loader callback task may make them. Works at any screen size the
 * brand fits, including 480 x 480 and 320 x 240.
 */

#include "h2_g99_loader_state.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_g99_loader_ui h2_g99_loader_ui_t;

/**
 * Opens Runtime Display and draws the brand alone. Requires Runtime display,
 * mem, task, sync, queue and time. Borrows `runtime` until close.
 */
h2_pal_result_t h2_g99_loader_ui_open(h2_runtime_t* runtime,
                                      h2_g99_loader_ui_t** out_ui);

/** Releases LVGL and closes the display it opened; NULL is ignored. */
void h2_g99_loader_ui_close(h2_g99_loader_ui_t* ui);

/** Each call updates the state (h2_g99_loader_state.h) and redraws. */
h2_pal_result_t h2_g99_loader_ui_ready(h2_g99_loader_ui_t* ui);
h2_pal_result_t h2_g99_loader_ui_installing(h2_g99_loader_ui_t* ui);
h2_pal_result_t h2_g99_loader_ui_progress(h2_g99_loader_ui_t* ui,
                                          h2_loader_install_phase_t phase,
                                          uint64_t completed, uint64_t total);
h2_pal_result_t h2_g99_loader_ui_failed(h2_g99_loader_ui_t* ui, int code);
h2_pal_result_t h2_g99_loader_ui_launching(h2_g99_loader_ui_t* ui);

/** Copies the state last drawn. */
void h2_g99_loader_ui_state(h2_g99_loader_ui_t* ui, h2_g99_loader_state_t* out);

#ifdef __cplusplus
}
#endif

#endif
