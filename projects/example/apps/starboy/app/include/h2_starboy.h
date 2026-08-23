#ifndef H2_STARBOY_H
#define H2_STARBOY_H

#include "h2_runtime.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Stable optional logical components mapped by target launchers. */
typedef enum h2_starboy_component_id {
    H2_STARBOY_COMPONENT_IMU = 1,
    H2_STARBOY_COMPONENT_THEME_BUTTON,
    H2_STARBOY_COMPONENT_POWER_BUTTON,
} h2_starboy_component_id_t;

/** Procedural pupil silhouettes selected independently from the colorway. */
typedef enum h2_starboy_pupil_style {
    H2_STARBOY_PUPIL_STYLE_DOT = 0,
    H2_STARBOY_PUPIL_STYLE_CIRCLE,
    H2_STARBOY_PUPIL_STYLE_CAT,
    H2_STARBOY_PUPIL_STYLE_ACORN,
    H2_STARBOY_PUPIL_STYLE_COUNT,
} h2_starboy_pupil_style_t;

/** Cooperative lifecycle callback. A nonzero result requests App exit. */
typedef int (*h2_starboy_should_stop_fn)(void *user);

/** Called once after the first complete frame has been presented. */
typedef h2_pal_result_t (*h2_starboy_ready_fn)(void *user);

/** Target-neutral raw motion sample used for gravity-referenced rendering. */
typedef struct h2_starboy_motion_sample {
    int32_t accel_mg[3];
    int32_t gyro_mdps[3];
} h2_starboy_motion_sample_t;

typedef struct h2_starboy_motion_vtable {
    h2_pal_result_t (*read)(
        void *user,
        h2_starboy_motion_sample_t *out_sample);
} h2_starboy_motion_vtable_t;

/** Optional target-private adapter for continuous raw motion samples. */
typedef struct h2_starboy_motion_api {
    void *user;
    const h2_starboy_motion_vtable_t *vtable;
} h2_starboy_motion_api_t;

/** Portable Starboy execution policy. */
typedef struct h2_starboy_config {
    /** Zero selects the default 16 ms animation interval. */
    uint32_t frame_interval_ms;
    /** Zero selects the App's deterministic default seed. */
    uint32_t random_seed;
    /** Optional continuous motion source; preferred over gesture-only IMU. */
    const h2_starboy_motion_api_t *motion;
    /** Nonzero consumes the fixed Starboy IMU component when motion is absent. */
    int enable_imu_component;
    /** Nonzero makes the theme Button generate a new high-contrast palette. */
    int enable_theme_button;
    /** Nonzero consumes the fixed Starboy power Button component. */
    int enable_power_button;
    /** Initial pupil silhouette; shake events cycle all four silhouettes. */
    h2_starboy_pupil_style_t initial_pupil_style;
    /** Optional readiness callback; failure stops the App and is returned. */
    h2_starboy_ready_fn ready;
    void *ready_user;
    /** Optional cooperative stop callback; NULL runs until an error. */
    h2_starboy_should_stop_fn should_stop;
    void *should_stop_user;
} h2_starboy_config_t;

/**
 * Runs Starboy using the supplied initialized Runtime.
 *
 * Display, Memory, and monotonic Time are required. Touch, Audio, gesture IMU,
 * continuous Motion, Button, and Power are optional and disable only their
 * corresponding behavior when unavailable.
 */
int h2_starboy_run(
    h2_runtime_t *runtime,
    const h2_starboy_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
