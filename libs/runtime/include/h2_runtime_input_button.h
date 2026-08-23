#ifndef H2_RUNTIME_INPUT_BUTTON_H
#define H2_RUNTIME_INPUT_BUTTON_H

/*
 * Scope: App-visible button event and state payloads.
 * Tunable macro defaults live in h2_runtime_input_button_defs.h.
 * Apps receive immediate down/up edges plus one BUTTON_ACTION per release
 * that carries the press/release timestamps and the consecutive click count.
 * Product code derives long-press and multi-click semantics from those
 * timestamps, the click count, and the pressed/pressed_at_ms state snapshot.
 */

#include "h2/pal/core/h2_pal_errors.h"
#include "h2_runtime_component.h"
#include "h2_runtime_input_button_defs.h"
#include "h2_runtime_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Immediate button-down edge payload. */
typedef struct h2_runtime_button_down_event {
    /** Runtime monotonic timestamp when the pressed state was first observed. */
    h2_runtime_timestamp_ms_t pressed_at_ms;
} h2_runtime_button_down_event_t;

/** Immediate button-up edge payload. */
typedef struct h2_runtime_button_up_event {
    /** Runtime monotonic timestamp of the matching button-down edge. */
    h2_runtime_timestamp_ms_t pressed_at_ms;
    /** Runtime monotonic timestamp when the released state was first observed. */
    h2_runtime_timestamp_ms_t released_at_ms;
} h2_runtime_button_up_event_t;

/**
 * Completed press/release action payload, emitted once per release.
 *
 * `released_at_ms - pressed_at_ms` is the press duration; `click_count` is the
 * one-based position in the current consecutive-click sequence (presses whose
 * gap does not exceed H2_RUNTIME_BUTTON_CLICK_GAP_MS keep counting).
 */
typedef struct h2_runtime_button_action_event {
    h2_runtime_timestamp_ms_t pressed_at_ms;
    h2_runtime_timestamp_ms_t released_at_ms;
    /** One-based count in the current consecutive-click sequence. */
    uint16_t click_count;
} h2_runtime_button_action_event_t;

/**
 * Raw button state snapshot.
 *
 * `click_count` mirrors the counter carried by BUTTON_ACTION: while pressed
 * it is the one-based position of the press in progress within the current
 * consecutive-click sequence; after release it keeps the count of the last
 * completed action until the next press starts or restarts the sequence.
 * It is 0 until the first press is observed.
 */
typedef struct h2_runtime_button_state {
    bool pressed;
    h2_runtime_timestamp_ms_t pressed_at_ms;
    uint16_t click_count;
    h2_runtime_timestamp_ms_t updated_at_ms;
    h2_pal_result_t result;
} h2_runtime_button_state_t;

/** One raw edge produced by a mapped PUSH_EDGE Button periph. */
typedef enum h2_runtime_button_edge {
    H2_RUNTIME_BUTTON_EDGE_DOWN = 1,
    H2_RUNTIME_BUTTON_EDGE_UP,
} h2_runtime_button_edge_t;

/**
 * Push one ordered raw edge for a mapped PUSH_EDGE Button periph.
 *
 * The producer identifies its PAL periph, not the App component. Runtime uses
 * the existing component mapping, owns timestamp/gesture/state/event
 * generation, and rejects unknown, unmapped, non-Button, or POLL_STATE
 * sources. Runtime serializes this operation with its input task and Runtime
 * Test Control. The adapter must call it from task/event-dispatch context, not
 * directly from an ISR, and must stop its producer before Runtime deinit.
 */
h2_pal_result_t h2_runtime_button_push_edge(
    h2_runtime_t *runtime,
    h2_pal_periph_id_t periph_id,
    h2_runtime_button_edge_t edge);

#ifdef __cplusplus
}
#endif

#endif
