#include "board_config.h"
#include "h2_esp_target_task_policy.h"

#include "h2_esp_board.h"
#include "h2_esp_board_config.h"
#include "h2_esp_platform_core.h"
#include "h2_starboy.h"

#include <stddef.h>

typedef struct h2_starboy_amoled_mapping {
    h2_runtime_component_t component;
    h2_runtime_component_mapping_entry_t entry;
} h2_starboy_amoled_mapping_t;

static const h2_starboy_amoled_mapping_t s_mappings[] = {
    {
        .component = H2_RUNTIME_COMPONENT_BUTTON,
        .entry = {
            .component_id = H2_STARBOY_COMPONENT_THEME_BUTTON,
            .periph_id = H2_AMOLED_BOOT_BUTTON_ID,
        },
    },
    {
        .component = H2_RUNTIME_COMPONENT_BUTTON,
        .entry = {
            .component_id = H2_STARBOY_COMPONENT_POWER_BUTTON,
            .periph_id = H2_AMOLED_POWER_BUTTON_ID,
        },
    },
};

static h2_pal_result_t motion_read(
    void *user,
    h2_starboy_motion_sample_t *out_sample) {
    if (user == NULL || out_sample == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_runtime_t *runtime = (h2_runtime_t *)user;
    h2_pal_imu_reading_t reading = {0};
    h2_pal_result_t rc = h2_pal_imu_read(
        runtime->imu,
        H2_AMOLED_QMI8658_MOTION_ID,
        &reading);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if ((reading.flags & (H2_PAL_IMU_HAS_ACCEL | H2_PAL_IMU_HAS_GYRO)) !=
        (H2_PAL_IMU_HAS_ACCEL | H2_PAL_IMU_HAS_GYRO)) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    /* QMI8658 is mounted 90 degrees clockwise from the portrait display. */
    *out_sample = (h2_starboy_motion_sample_t){
        .accel_mg = {
            reading.accel_mg.y,
            -reading.accel_mg.x,
            reading.accel_mg.z,
        },
        .gyro_mdps = {
            reading.gyro_mdps.y,
            -reading.gyro_mdps.x,
            reading.gyro_mdps.z,
        },
    };
    return H2_PAL_OK;
}

static const h2_starboy_motion_vtable_t s_motion_vtable = {
    .read = motion_read,
};

static h2_pal_result_t mapper_list(
    void *user,
    h2_runtime_component_t filter,
    h2_runtime_component_mapping_cb_t callback,
    void *callback_user) {
    (void)user;
    if (callback == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t index = 0u;
         index < sizeof(s_mappings) / sizeof(s_mappings[0]);
         ++index) {
        if (filter != H2_RUNTIME_COMPONENT_NONE &&
            filter != s_mappings[index].component) {
            continue;
        }
        h2_pal_result_t rc = callback(
            callback_user, &s_mappings[index].entry);
        if (rc != H2_PAL_OK) {
            return rc;
        }
    }
    return H2_PAL_OK;
}

static h2_pal_result_t mapper_get(
    void *user,
    h2_runtime_component_id_t component_id,
    h2_pal_periph_id_t *out_periph_id) {
    (void)user;
    if (out_periph_id == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t index = 0u;
         index < sizeof(s_mappings) / sizeof(s_mappings[0]);
         ++index) {
        if (component_id == s_mappings[index].entry.component_id) {
            *out_periph_id = s_mappings[index].entry.periph_id;
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static const h2_runtime_component_mapper_vtable_t s_mapper_vtable = {
    .list = mapper_list,
    .get_periph_id = mapper_get,
};

static const h2_runtime_component_mapper_t s_mapper = {
    .user = NULL,
    .vtable = &s_mapper_vtable,
};

h2_pal_result_t h2_starboy_amoled_runtime_config(
    h2_runtime_config_t *out_config) {
    h2_pal_result_t rc = h2_esp_target_task_policy_install();
    if (rc != H2_PAL_OK) {
        return rc;
    }
    rc = h2_esp_board_runtime_config(out_config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    out_config->component_mapper = &s_mapper;
    out_config->mem = h2_esp_platform_psram_allocator();
    out_config->event_queue_capacity =
        H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY;
    return H2_PAL_OK;
}

h2_pal_result_t h2_starboy_amoled_input_poll_config(
    h2_runtime_input_poll_config_t *out_config) {
    if (out_config == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_config = (h2_runtime_input_poll_config_t){
        .button_poll_interval_ms = 20u,
    };
    return H2_PAL_OK;
}

h2_starboy_motion_api_t h2_starboy_amoled_motion_api(
    h2_runtime_t *runtime) {
    return (h2_starboy_motion_api_t){
        .user = runtime,
        .vtable = &s_motion_vtable,
    };
}
