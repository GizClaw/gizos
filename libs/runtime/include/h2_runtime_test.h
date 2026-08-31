#ifndef H2_RUNTIME_TEST_H
#define H2_RUNTIME_TEST_H

/*
 * Scope: Host and device-test control for injecting recognized public Runtime
 * events and component state. Production Apps must not receive this control.
 */

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_runtime_test_control h2_runtime_test_control_t;

/**
 * Opens the Runtime-owned test-input session.
 *
 * Opening pauses the Runtime-owned input task at its serialized writer
 * boundary, clears queued production events and the physical source cache,
 * publishes a neutral snapshot for mapped input components, and gives the
 * control exclusive writer ownership. The caller must close it before Runtime
 * deinitialization.
 */
h2_pal_result_t h2_runtime_test_control_open(
    h2_runtime_t *runtime,
    h2_runtime_test_control_t **out_control);

/**
 * Copies one validated public Runtime event into the production event queue.
 *
 * Sequence allocation and queue-full/drop behavior are identical to other
 * Runtime producers. Use the typed button helpers when button state and event
 * must change consistently.
 */
h2_pal_result_t h2_runtime_test_emit_event(
    h2_runtime_test_control_t *control,
    h2_runtime_event_kind_t kind,
    h2_runtime_component_t component,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t timestamp_ms,
    const void *payload,
    size_t payload_size);

/**
 * Publishes one mapped input component state through Runtime snapshot storage.
 *
 * The component id must be present in the Runtime component mapping and the
 * state size must match its Button, NFC, or IMU public state type.
 */
h2_pal_result_t h2_runtime_test_set_component_state(
    h2_runtime_test_control_t *control,
    h2_runtime_component_id_t component_id,
    const void *state,
    size_t state_size);

/** Refreshes non-event Battery and Temperature snapshots from their PAL. */
h2_pal_result_t h2_runtime_test_poll_sensors(h2_runtime_t *runtime);

h2_pal_result_t h2_runtime_test_button_down(
    h2_runtime_test_control_t *control,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t pressed_at_ms);

h2_pal_result_t h2_runtime_test_button_up(
    h2_runtime_test_control_t *control,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t pressed_at_ms,
    h2_runtime_timestamp_ms_t released_at_ms);

h2_pal_result_t h2_runtime_test_button_action(
    h2_runtime_test_control_t *control,
    h2_runtime_component_id_t component_id,
    h2_runtime_timestamp_ms_t pressed_at_ms,
    h2_runtime_timestamp_ms_t released_at_ms,
    uint16_t click_count);

/** Inject one objective Button Action phase and matching state snapshot. */
h2_pal_result_t h2_runtime_test_button_action_phase(
    h2_runtime_test_control_t *control,
    h2_runtime_component_id_t component_id,
    h2_runtime_button_action_phase_t phase,
    h2_runtime_timestamp_ms_t pressed_at_ms,
    h2_runtime_timestamp_ms_t observed_at_ms,
    uint16_t click_count);

/**
 * Closes the exclusive test-input session and publishes an empty snapshot.
 *
 * The next Runtime input tick lazily discovers the physical sources again.
 */
void h2_runtime_test_control_close(h2_runtime_test_control_t *control);

#ifdef __cplusplus
}
#endif

#endif
