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

/** Lifecycle phase of Runtime-owned input acquisition. */
typedef enum h2_runtime_input_phase {
    H2_RUNTIME_INPUT_PHASE_STOPPED = 0,
    H2_RUNTIME_INPUT_PHASE_STARTING,
    H2_RUNTIME_INPUT_PHASE_TASK_RUNNING,
    H2_RUNTIME_INPUT_PHASE_STOPPING,
    /** A fatal worker error stopped acquisition and closed the event queue. */
    H2_RUNTIME_INPUT_PHASE_FAULTED,
} h2_runtime_input_phase_t;

/** The input-worker step that produced an error. */
typedef enum h2_runtime_input_stage {
    H2_RUNTIME_INPUT_STAGE_NONE = 0,
    H2_RUNTIME_INPUT_STAGE_LOCK,
    H2_RUNTIME_INPUT_STAGE_TIME,
    H2_RUNTIME_INPUT_STAGE_SOURCES,
    H2_RUNTIME_INPUT_STAGE_PUSH_EDGE,
    H2_RUNTIME_INPUT_STAGE_NFC_RESULT,
    H2_RUNTIME_INPUT_STAGE_BUTTON,
    H2_RUNTIME_INPUT_STAGE_RADIO_BUTTON,
    H2_RUNTIME_INPUT_STAGE_IMU,
    H2_RUNTIME_INPUT_STAGE_BATTERY,
    H2_RUNTIME_INPUT_STAGE_TEMPERATURE,
    H2_RUNTIME_INPUT_STAGE_EVENT_PUBLISH,
    H2_RUNTIME_INPUT_STAGE_SNAPSHOT_PUBLISH,
    H2_RUNTIME_INPUT_STAGE_SLEEP,
    H2_RUNTIME_INPUT_STAGE_NFC_SCAN,
} h2_runtime_input_stage_t;

/**
 * Health of Runtime-owned input acquisition.
 *
 * A failed poll step is recoverable: the worker records it here, logs it with
 * its stage, backs off and polls again, so one bad ADC read or a momentarily
 * full buffer never silences the buttons. Only a failure that leaves the
 * worker unable to pace itself (sleep) or an unusable Runtime is fatal. A
 * fatal stop releases every held Button through the normal release path, so
 * its state reads released, and queues BUTTON_UP and a released BUTTON_ACTION
 * ahead of the close, waiting up to 100 ms per event for queue space; events
 * the queue still does not take are counted in the `H2_RUNTIME_INPUT_FAULT`
 * log line. If the input writer mutex itself cannot be taken, the release is
 * skipped and the log line says so. The stop then latches `worker_result`,
 * closes the Runtime event queue and moves the phase to FAULTED.
 *
 * The health fields have their own lock, separate from the input writer
 * mutex, so a failure to take the writer mutex is recorded too (stage
 * `lock`).
 */
typedef struct h2_runtime_input_status {
    h2_runtime_input_phase_t phase;
    /** Latched fatal result; `H2_PAL_OK` while the worker never faulted. */
    h2_pal_result_t worker_result;
    /** Most recent failed step, recoverable or fatal. */
    h2_pal_result_t last_error;
    h2_runtime_input_stage_t last_error_stage;
    h2_runtime_timestamp_ms_t last_error_at_ms;
    /** Failed polls since start, and failed polls since the last good one. */
    uint32_t error_count;
    uint32_t consecutive_error_count;
    /** Worker polls that completed without error, and when the last did. */
    uint32_t poll_count;
    h2_runtime_timestamp_ms_t last_poll_ok_at_ms;
    /** State publications deferred because readers pinned every slot. */
    uint32_t snapshot_deferred_count;
} h2_runtime_input_status_t;

/**
 * Read the health of Runtime-owned input acquisition.
 *
 * Safe to call from any task at any phase, including after a fault. Counters
 * accumulate for the life of the Runtime. The copy is taken under the health
 * lock and is consistent with itself; `phase` and `worker_result` are read
 * atomically alongside it.
 *
 * @param runtime Initialized Runtime instance.
 * @param out_status Receives the status.
 * @return `H2_PAL_OK`; `H2_PAL_ERR_INVALID_ARG` for an unusable Runtime or a
 *         null output; the Sync PAL result when the health lock cannot be
 *         taken, in which case `out_status` is left untouched.
 */
h2_pal_result_t h2_runtime_input_status(
    h2_runtime_t *runtime,
    h2_runtime_input_status_t *out_status);

/** Stable lowercase name of a stage, for logs. */
const char *h2_runtime_input_stage_name(h2_runtime_input_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif
