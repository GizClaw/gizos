#ifndef H2_G99_LOADER_CONTROLLER_H
#define H2_G99_LOADER_CONTROLLER_H

#include "h2_g99_loader_state.h"
#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_g99_loader_controller h2_g99_loader_controller_t;

/**
 * Starts the sole Loader UI task and waits for the initial brand frame.
 * Runtime and its providers remain borrowed until close clears the handle.
 * On failure a non-NULL output retains cleanup ownership; call close again.
 */
h2_pal_result_t h2_g99_loader_controller_open(
    h2_runtime_t* runtime, h2_g99_loader_controller_t** out_controller);

/**
 * Copies one event, serializes concurrent callers and waits for its rendering.
 * Call only from task context, outside the UI task. Rendering failures
 * propagate. Launching returns only after the frame has been presented.
 */
h2_pal_result_t h2_g99_loader_controller_installing(
    h2_g99_loader_controller_t* controller);
h2_pal_result_t h2_g99_loader_controller_progress(
    h2_g99_loader_controller_t* controller, h2_loader_install_phase_t phase,
    uint64_t completed, uint64_t total);
h2_pal_result_t h2_g99_loader_controller_failed(
    h2_g99_loader_controller_t* controller, int code);
h2_pal_result_t h2_g99_loader_controller_launching(
    h2_g99_loader_controller_t* controller);
h2_pal_result_t h2_g99_loader_controller_state(
    h2_g99_loader_controller_t* controller, h2_g99_loader_state_t* out_state);

/**
 * Stop callers before closing. Closes admission, joins the UI task and releases
 * its resources. Failure retains the handle and borrowed Runtime for retry.
 */
h2_pal_result_t h2_g99_loader_controller_close(
    h2_g99_loader_controller_t** controller);

#ifdef __cplusplus
}
#endif

#endif
