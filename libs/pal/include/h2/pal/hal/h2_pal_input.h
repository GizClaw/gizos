#ifndef H2_PAL_INPUT_H
#define H2_PAL_INPUT_H

#include "h2/pal/hal/h2_pal_button.h"
#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/hal/h2_pal_nfc.h"
#include "h2/pal/hal/h2_pal_periph.h"
#include "h2/pal/core/h2_pal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_motion_flags {
    H2_PAL_MOTION_FLAG_NONE = 0,
    H2_PAL_MOTION_HAS_ACCEL = 1u << 0,
    H2_PAL_MOTION_HAS_GYRO = 1u << 1,
    H2_PAL_MOTION_HAS_MAG = 1u << 2,
} h2_pal_motion_flags_t;

typedef struct h2_pal_motion_reading {
    h2_pal_periph_id_t id;
    uint32_t flags;
    h2_pal_vec3_i32_t accel_mg;
    h2_pal_vec3_i32_t gyro_mdps;
    h2_pal_vec3_i32_t mag_mgauss;
} h2_pal_motion_reading_t;

typedef enum h2_pal_battery_flags {
    H2_PAL_BATTERY_FLAG_NONE = 0,
    H2_PAL_BATTERY_HAS_VOLTAGE_MV = 1u << 0,
    H2_PAL_BATTERY_HAS_PERCENT_X100 = 1u << 1,
    H2_PAL_BATTERY_HAS_CURRENT_MA = 1u << 2,
    H2_PAL_BATTERY_PRESENT = 1u << 3,
    H2_PAL_BATTERY_CHARGING = 1u << 4,
    H2_PAL_BATTERY_FULL = 1u << 5,
    H2_PAL_BATTERY_LOW = 1u << 6,
} h2_pal_battery_flags_t;

typedef struct h2_pal_battery_reading {
    h2_pal_periph_id_t id;
    uint32_t flags;
    int32_t voltage_mv;
    int32_t current_ma;
    uint16_t percent_x100;
} h2_pal_battery_reading_t;

typedef enum h2_pal_temperature_flags {
    H2_PAL_TEMPERATURE_FLAG_NONE = 0,
    H2_PAL_TEMPERATURE_HAS_MILLI_CELSIUS = 1u << 0,
} h2_pal_temperature_flags_t;

typedef struct h2_pal_temperature_reading {
    h2_pal_periph_id_t id;
    uint32_t flags;
    int32_t milli_celsius;
} h2_pal_temperature_reading_t;

typedef struct h2_pal_input_vtable {
    h2_pal_result_t (*read_motion)(
        void *user,
        h2_pal_periph_id_t id,
        h2_pal_motion_reading_t *out_reading);

    h2_pal_result_t (*read_battery)(
        void *user,
        h2_pal_periph_id_t id,
        h2_pal_battery_reading_t *out_reading);

    h2_pal_result_t (*read_temperature)(
        void *user,
        h2_pal_periph_id_t id,
        h2_pal_temperature_reading_t *out_reading);
} h2_pal_input_vtable_t;

typedef struct h2_pal_input_api {
    void *user;
    const h2_pal_input_vtable_t *vtable;
} h2_pal_input_api_t;

static inline h2_pal_result_t h2_pal_input_read_motion(
    const h2_pal_input_api_t *api,
    h2_pal_periph_id_t id,
    h2_pal_motion_reading_t *out_reading) {
    if (out_reading == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->read_motion == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->read_motion(api->user, id, out_reading);
}

static inline h2_pal_result_t h2_pal_input_read_battery(
    const h2_pal_input_api_t *api,
    h2_pal_periph_id_t id,
    h2_pal_battery_reading_t *out_reading) {
    if (out_reading == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->read_battery == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->read_battery(api->user, id, out_reading);
}

static inline h2_pal_result_t h2_pal_input_read_temperature(
    const h2_pal_input_api_t *api,
    h2_pal_periph_id_t id,
    h2_pal_temperature_reading_t *out_reading) {
    if (out_reading == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->read_temperature == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->read_temperature(api->user, id, out_reading);
}

#ifdef __cplusplus
}
#endif

#endif
