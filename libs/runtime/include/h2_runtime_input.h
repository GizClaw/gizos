#ifndef H2_RUNTIME_INPUT_H
#define H2_RUNTIME_INPUT_H

/*
 * Scope: Runtime input poll configuration and the caller-owned poller switch.
 * Start and stop are symmetric and repeatable, and they only switch the
 * poller: component state, the input source table, the writer mutex and the
 * state publication all belong to the Runtime and live from init to deinit.
 */

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_task.h"
#include "h2_runtime_types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_runtime_input_poll_config {
    /** Worker wake cadence; zero selects the Runtime button interval. */
    uint32_t tick_ms;
    /** Per-source cadences; zero selects each Runtime capability default. */
    uint32_t button_poll_interval_ms;
    uint32_t nfc_poll_interval_ms;
    uint32_t imu_poll_interval_ms;
    uint32_t battery_poll_interval_ms;
    uint32_t temperature_poll_interval_ms;
    /** Target task policy; a null name selects `h2-runtime-input`. */
    h2_pal_task_options_t task_options;
} h2_runtime_input_poll_config_t;

/**
 * Start Runtime-owned input acquisition.
 *
 * `h2_runtime_init()` does not start the poller. The caller that owns the
 * Runtime lifecycle starts it explicitly once component validation has
 * passed, and uses the same entry point to resume after
 * `h2_runtime_input_stop()`. A Runtime whose poller is never started delivers
 * no Button, NFC, IMU or sensor event.
 *
 * A start selects the poll cadences and task policy, takes one immediate
 * frame so the next publication reflects current hardware, and then starts
 * the private input task (plus the NFC task when an NFC reader is mapped).
 * The input source table, the writer mutex and the state publication are
 * built once by `h2_runtime_init()`; a start does not rebuild them and does
 * not reset component state or Button action state.
 *
 * @param runtime Initialized Runtime instance.
 * @param config Poll cadences and task policy; a null pointer selects every
 *               Runtime default. The config is copied and not retained.
 * @return `H2_PAL_OK` when the poller is running, or when the Runtime has no
 *         mapped input source and therefore needs no input task.
 *         `H2_PAL_ERR_INVALID_ARG` for an unusable Runtime.
 *         `H2_PAL_ERR_INVALID_STATE` when acquisition is already running, when
 *         a test-control session is open, or when a previous worker fault has
 *         closed the Runtime event queue; a faulted Runtime is terminal and
 *         only `h2_runtime_deinit()` plus a fresh `h2_runtime_init()` recovers
 *         it. Any other PAL result comes from the failing PAL operation, and
 *         leaves acquisition stopped.
 */
h2_pal_result_t h2_runtime_input_start(
    h2_runtime_t *runtime,
    const h2_runtime_input_poll_config_t *config);

/**
 * Stop Runtime-owned input acquisition.
 *
 * Requests the input and NFC tasks to stop and joins them. That is all it
 * does: component state is untouched, so `h2_runtime_component_state_*()`
 * keeps returning the last published snapshot with its own `updated_at_ms`,
 * which is still the most recent observation the Runtime actually made.
 * Queued push edges are kept and delivered by a later start.
 *
 * Because state is not reset, a button released while the poller was off is
 * observed only by the next start's first frame, which then reports a
 * `BUTTON_ACTION` spanning the whole stopped interval, and the consecutive
 * click sequence continues across the stop. Callers that care must decide
 * that for themselves.
 *
 * `h2_runtime_deinit()` performs this stop itself, so a caller only needs it
 * to park a still-powered device with the poller off.
 *
 * @param runtime Initialized Runtime instance.
 * @return `H2_PAL_OK` when the poller is stopped, including a repeated stop.
 *         `H2_PAL_ERR_INVALID_ARG` for an unusable Runtime.
 *         `H2_PAL_ERR_INVALID_STATE` while a start or stop is in progress. The
 *         latched worker result when a background worker faulted before the
 *         stop. A join failure returns the PAL result and leaves the poller
 *         running so the caller can retry.
 */
h2_pal_result_t h2_runtime_input_stop(h2_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
