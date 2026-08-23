#ifndef H2_RUNTIME_INPUT_IMU_H
#define H2_RUNTIME_INPUT_IMU_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2_runtime_component.h"
#include "h2_runtime_input_imu_defs.h"
#include "h2_runtime_input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_runtime_imu_gesture_kind {
    H2_RUNTIME_IMU_GESTURE_NONE = 0,
    H2_RUNTIME_IMU_GESTURE_SHAKE,
    H2_RUNTIME_IMU_GESTURE_TILT,
    H2_RUNTIME_IMU_GESTURE_FLIP,
    H2_RUNTIME_IMU_GESTURE_FREE_FALL,
} h2_runtime_imu_gesture_kind_t;

typedef struct h2_runtime_imu_shake {
    int32_t magnitude_mg;
    uint32_t duration_ms;
} h2_runtime_imu_shake_t;

typedef struct h2_runtime_imu_tilt {
    int32_t x_mg;
    int32_t y_mg;
    int32_t z_mg;
} h2_runtime_imu_tilt_t;

typedef struct h2_runtime_imu_flip {
    int32_t gyro_z_mdps;
} h2_runtime_imu_flip_t;

typedef struct h2_runtime_imu_free_fall {
    uint32_t duration_ms;
    int32_t magnitude_mg;
} h2_runtime_imu_free_fall_t;

typedef struct h2_runtime_imu_gesture_event {
    h2_runtime_imu_gesture_kind_t kind;
    union {
        h2_runtime_imu_shake_t shake;
        h2_runtime_imu_tilt_t tilt;
        h2_runtime_imu_flip_t flip;
        h2_runtime_imu_free_fall_t free_fall;
    } gesture;
} h2_runtime_imu_gesture_event_t;

typedef struct h2_runtime_imu_state {
    h2_runtime_imu_gesture_kind_t gesture_kind;
    h2_runtime_timestamp_ms_t updated_at_ms;
    h2_pal_result_t result;
    union {
        h2_runtime_imu_shake_t shake;
        h2_runtime_imu_tilt_t tilt;
        h2_runtime_imu_flip_t flip;
        h2_runtime_imu_free_fall_t free_fall;
    } gesture;
} h2_runtime_imu_state_t;

#ifdef __cplusplus
}
#endif

#endif
