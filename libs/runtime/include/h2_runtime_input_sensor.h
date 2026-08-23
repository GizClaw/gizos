#ifndef H2_RUNTIME_INPUT_SENSOR_H
#define H2_RUNTIME_INPUT_SENSOR_H

/* Scope: App-visible Battery and Temperature snapshot payloads. */

#include "h2/pal/hal/h2_pal_input.h"
#include "h2_runtime_input_sensor_defs.h"
#include "h2_runtime_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_runtime_battery_state {
    h2_pal_battery_reading_t reading;
    h2_pal_result_t result;
    h2_runtime_timestamp_ms_t updated_at_ms;
} h2_runtime_battery_state_t;

typedef struct h2_runtime_temperature_state {
    h2_pal_temperature_reading_t reading;
    h2_pal_result_t result;
    h2_runtime_timestamp_ms_t updated_at_ms;
} h2_runtime_temperature_state_t;

#ifdef __cplusplus
}
#endif

#endif
