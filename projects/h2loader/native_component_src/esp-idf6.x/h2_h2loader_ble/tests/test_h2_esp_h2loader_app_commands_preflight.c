#include "h2_esp_h2loader_app_commands_preflight.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct test_env {
    int create_count;
    int destroy_count;
    h2_pal_result_t firmware_result;
    h2_pal_result_t power_result;
    h2_pal_power_boot_partition_t partition;
} test_env_t;

static h2_pal_result_t create_mutex(
    void *user, const h2_pal_mutex_config_t *config,
    h2_pal_mutex_t **out_mutex) {
    test_env_t *env = user;
    assert(config != NULL);
    assert(strcmp(config->name, "h2loader-app-operation") == 0);
    assert(config->flags == H2_PAL_MUTEX_FLAG_RECURSIVE);
    ++env->create_count;
    *out_mutex = (h2_pal_mutex_t *)env;
    return H2_PAL_OK;
}

static h2_pal_result_t destroy_mutex(void *user, h2_pal_mutex_t *mutex) {
    test_env_t *env = user;
    assert(mutex == (h2_pal_mutex_t *)env);
    ++env->destroy_count;
    return H2_PAL_OK;
}

static h2_pal_result_t get_firmware(
    void *user, h2_pal_firmware_info_t *out_info) {
    test_env_t *env = user;
    if (env->firmware_result == H2_PAL_OK)
        strcpy(out_info->version, "1.2.3");
    return env->firmware_result;
}

static h2_pal_result_t get_running_partition(
    void *user, h2_pal_power_boot_partition_t *out_partition) {
    test_env_t *env = user;
    if (env->power_result == H2_PAL_OK)
        *out_partition = env->partition;
    return env->power_result;
}

static const h2_pal_sync_vtable_t sync_vtable = {
    .create_mutex = create_mutex,
    .destroy_mutex = destroy_mutex,
};

static const h2_pal_firmware_info_vtable_t firmware_vtable = {
    .get_current = get_firmware,
};

static const h2_pal_power_vtable_t power_vtable = {
    .get_running_boot_partition = get_running_partition,
};

static h2_runtime_config_t runtime_config(test_env_t *env) {
    static const h2_pal_mem_api_t allocator = {0};
    static h2_pal_sync_api_t sync;
    static h2_pal_firmware_info_api_t firmware;
    static h2_pal_power_api_t power;
    sync = (h2_pal_sync_api_t){.user = env, .vtable = &sync_vtable};
    firmware = (h2_pal_firmware_info_api_t){
        .user = env,
        .vtable = &firmware_vtable,
    };
    power = (h2_pal_power_api_t){.user = env, .vtable = &power_vtable};
    return (h2_runtime_config_t){
        .mem = &allocator,
        .sync = &sync,
        .firmware_info = &firmware,
        .power = &power,
    };
}

static void expect_failure(
    test_env_t *env, h2_pal_result_t expected_result) {
    const h2_runtime_config_t config = runtime_config(env);
    h2_esp_h2loader_app_commands_preflight_t preflight;
    assert(h2_esp_h2loader_app_commands_preflight(&config, &preflight) ==
           expected_result);
    assert(env->create_count == 1);
    assert(env->destroy_count == 1);
    assert(preflight.operation_mutex == NULL);
    assert(preflight.app_partition_id == 0u);
}

int main(void) {
    test_env_t firmware_failure = {
        .firmware_result = H2_PAL_ERR_IO,
        .power_result = H2_PAL_OK,
    };
    expect_failure(&firmware_failure, H2_PAL_ERR_IO);

    test_env_t provider_failure = {
        .firmware_result = H2_PAL_OK,
        .power_result = H2_PAL_ERR_IO,
    };
    expect_failure(&provider_failure, H2_PAL_ERR_IO);

    test_env_t zero_id = {
        .firmware_result = H2_PAL_OK,
        .power_result = H2_PAL_OK,
        .partition = {.flags = H2_PAL_POWER_BOOT_PARTITION_FLAG_APP},
    };
    expect_failure(&zero_id, H2_PAL_ERR_INVALID_STATE);

    test_env_t non_app = {
        .firmware_result = H2_PAL_OK,
        .power_result = H2_PAL_OK,
        .partition = {.id = 2u},
    };
    expect_failure(&non_app, H2_PAL_ERR_INVALID_STATE);

    test_env_t valid = {
        .firmware_result = H2_PAL_OK,
        .power_result = H2_PAL_OK,
        .partition = {
            .id = 2u,
            .flags = H2_PAL_POWER_BOOT_PARTITION_FLAG_APP,
        },
    };
    const h2_runtime_config_t config = runtime_config(&valid);
    h2_esp_h2loader_app_commands_preflight_t preflight;
    assert(h2_esp_h2loader_app_commands_preflight(&config, &preflight) ==
           H2_PAL_OK);
    assert(valid.create_count == 1);
    assert(valid.destroy_count == 0);
    assert(preflight.operation_mutex == (h2_pal_mutex_t *)&valid);
    assert(preflight.app_partition_id == 2u);
    assert(strcmp(preflight.firmware_info.version, "1.2.3") == 0);
    assert(h2_pal_mutex_destroy(config.sync, preflight.operation_mutex) ==
           H2_PAL_OK);
    assert(valid.destroy_count == 1);

    puts("h2loader App command preflight tests passed");
    return 0;
}
