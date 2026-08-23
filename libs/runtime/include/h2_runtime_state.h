#ifndef H2_RUNTIME_STATE_H
#define H2_RUNTIME_STATE_H

/*
 * Scope: Read-only runtime state API.
 *
 * State reads return caller-owned copies from the last completed state
 * publication. Multiple readers may overlap. A read never observes a poll
 * cycle still in progress and does not retain Runtime-owned storage.
 */

#include "h2/pal/core/h2_pal_errors.h"
#include "h2_runtime_component.h"
#include "h2_runtime_input_button.h"
#include "h2_runtime_input_imu.h"
#include "h2_runtime_input_nfc.h"
#include "h2_runtime_input_sensor.h"
#include "h2_runtime_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Default minimum interval between state publications triggered by changes
 * that carry no event. Overridable per target through
 * h2_runtime_config_t.state.publish_interval_ms.
 */
#ifndef H2_RUNTIME_STATE_PUBLISH_INTERVAL_MS
#define H2_RUNTIME_STATE_PUBLISH_INTERVAL_MS 40u
#endif

/** Target-owned policy for the Runtime state publication. */
typedef struct h2_runtime_state_config {
    /**
     * Minimum interval between state publications for changes that carry no
     * event (sensor readings, held-button timestamps); zero selects
     * H2_RUNTIME_STATE_PUBLISH_INTERVAL_MS. Changes that emit an event
     * publish immediately regardless of this cadence. This is not a poll
     * interval; input cadences live in h2_runtime_input_poll_config_t.
     */
    uint32_t publish_interval_ms;
} h2_runtime_state_config_t;

typedef struct h2_runtime_component_state_display {
    int reserved;
} h2_runtime_component_state_display_t;

typedef struct h2_runtime_component_state_audio {
    int reserved;
} h2_runtime_component_state_audio_t;

typedef struct h2_runtime_component_state_power {
    int reserved;
} h2_runtime_component_state_power_t;

typedef struct h2_runtime_component_state_netif {
    int reserved;
} h2_runtime_component_state_netif_t;

typedef struct h2_runtime_component_state_led {
    int reserved;
} h2_runtime_component_state_led_t;

typedef struct h2_runtime_component_state_switch {
    int reserved;
} h2_runtime_component_state_switch_t;

typedef struct h2_runtime_component_state_pwm_switch {
    int reserved;
} h2_runtime_component_state_pwm_switch_t;

/**
 * Copies the latest completed button snapshot into caller-owned storage.
 *
 * The output is zeroed on every failure. Before the first successful input
 * start and after a completed stop, this returns H2_PAL_ERR_NOT_FOUND.
 */
h2_pal_result_t h2_runtime_component_state_button(const h2_runtime_t *runtime, h2_runtime_component_id_t component_id, h2_runtime_button_state_t *out_state);

/** Copies the latest completed NFC snapshot with the same read semantics. */
h2_pal_result_t h2_runtime_component_state_nfc(const h2_runtime_t *runtime, h2_runtime_component_id_t component_id, h2_runtime_nfc_state_t *out_state);

/** Copies the latest completed IMU snapshot with the same read semantics. */
h2_pal_result_t h2_runtime_component_state_imu(const h2_runtime_t *runtime, h2_runtime_component_id_t component_id, h2_runtime_imu_state_t *out_state);
/** Copies the latest completed Battery snapshot with the same read semantics. */
h2_pal_result_t h2_runtime_component_state_battery(const h2_runtime_t *runtime, h2_runtime_component_id_t component_id, h2_runtime_battery_state_t *out_state);
/** Copies the latest completed Temperature snapshot with the same read semantics. */
h2_pal_result_t h2_runtime_component_state_temperature(const h2_runtime_t *runtime, h2_runtime_component_id_t component_id, h2_runtime_temperature_state_t *out_state);
h2_pal_result_t h2_runtime_component_state_display(const h2_runtime_t *runtime, h2_runtime_component_state_display_t *out_state);
h2_pal_result_t h2_runtime_component_state_audio(const h2_runtime_t *runtime, h2_runtime_component_state_audio_t *out_state);
h2_pal_result_t h2_runtime_component_state_power(const h2_runtime_t *runtime, h2_runtime_component_state_power_t *out_state);
h2_pal_result_t h2_runtime_component_state_netif(const h2_runtime_t *runtime, h2_runtime_component_state_netif_t *out_state);
h2_pal_result_t h2_runtime_component_state_led(const h2_runtime_t *runtime, h2_runtime_component_state_led_t *out_state);
h2_pal_result_t h2_runtime_component_state_switch(const h2_runtime_t *runtime, h2_runtime_component_state_switch_t *out_state);
h2_pal_result_t h2_runtime_component_state_pwm_switch(const h2_runtime_t *runtime, h2_runtime_component_state_pwm_switch_t *out_state);

#ifdef __cplusplus
}
#endif

#endif
