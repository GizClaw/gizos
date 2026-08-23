#ifndef H2_PAL_IMU_H
#define H2_PAL_IMU_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/hal/h2_pal_periph.h"
#include "h2/pal/core/h2_pal_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_pal_imu_flags {
    H2_PAL_IMU_FLAG_NONE = 0,
    H2_PAL_IMU_HAS_ACCEL = 1u << 0,
    H2_PAL_IMU_HAS_GYRO = 1u << 1,
    H2_PAL_IMU_HAS_MAG = 1u << 2,
} h2_pal_imu_flags_t;

typedef struct h2_pal_imu_reading {
    h2_pal_periph_id_t id;
    uint32_t flags;
    h2_pal_vec3_i32_t accel_mg;
    h2_pal_vec3_i32_t gyro_mdps;
    h2_pal_vec3_i32_t mag_mgauss;
} h2_pal_imu_reading_t;

typedef struct h2_pal_imu_vtable {
    h2_pal_result_t (*read_imu)(
        void *user,
        h2_pal_periph_id_t id,
        h2_pal_imu_reading_t *out_reading);
} h2_pal_imu_vtable_t;

typedef struct h2_pal_imu_api {
    void *user;
    const h2_pal_imu_vtable_t *vtable;
} h2_pal_imu_api_t;

static inline h2_pal_result_t h2_pal_imu_read(
    const h2_pal_imu_api_t *api,
    h2_pal_periph_id_t id,
    h2_pal_imu_reading_t *out_reading) {
    if (out_reading == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (api == NULL || api->vtable == NULL || api->vtable->read_imu == NULL) {
        return H2_PAL_ERR_UNSUPPORTED;
    }
    return api->vtable->read_imu(api->user, id, out_reading);
}

#ifdef __cplusplus
}
#endif

#endif
