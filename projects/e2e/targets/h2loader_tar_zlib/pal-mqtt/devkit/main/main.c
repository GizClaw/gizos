#include "h2_esp_board.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include "h2_coremqtt.h"
#include "device_runner.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include <stdio.h>
#include <string.h>

static h2_runtime_t *runtime;
static h2_coremqtt_t *mqtt;
static h2_pal_mqtt_api_t mqtt_api;
static h2_mqtt_device_result_t result;
static size_t owner_bytes;

static void hold(void) { for (;;) vTaskDelay(pdMS_TO_TICKS(1000u)); }
static void fail(const char *stage, int rc) {
    printf("H2_PAL_MQTT_SETUP_FAIL stage=%s rc=%d\n", stage, rc);
    fflush(stdout);
    hold();
}
static int snapshot(size_t out[10]) {
    h2_esp_platform_resource_stats_t stats = {0};
    int rc = h2_esp_platform_get_resource_stats(&stats);
    if (rc == H2_PAL_OK) {
        const size_t values[] = {stats.live_tasks, stats.task_stack_bytes,
            stats.live_queues, stats.live_mutexes, stats.live_semaphores,
            stats.live_conditions, stats.live_timers, stats.live_firmware_infos,
            stats.allocations, stats.allocation_bytes};
        memcpy(out, values, sizeof(values));
    }
    return rc;
}
static int ca_digest(void *user, const uint8_t *bytes, size_t length, uint8_t digest[32]) {
    (void)user;
    size_t written = 0u;
    if (psa_crypto_init() != PSA_SUCCESS) return H2_PAL_ERR_IO;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, length, digest, 32u, &written) == PSA_SUCCESS &&
        written == 32u ? H2_PAL_OK : H2_PAL_ERR_IO;
}
static void run(void *user) {
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(5000u));
    int rc;
    for (;;) {
        rc = h2_mqtt_device_prepare(runtime);
        if (rc == H2_PAL_OK) break;
        if (rc != H2_PAL_ERR_NOT_FOUND && rc != H2_PAL_ERR_UNAVAILABLE &&
            rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_BUSY) fail("network", rc);
        printf("H2_PAL_MQTT_SETUP_WAIT network=not_ready rc=%d cases_started=0\n", rc);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(3000u));
    }
    rc = snapshot(result.before);
    if (rc != H2_PAL_OK) fail("before", rc);
    rc = h2_mqtt_device_run(runtime, 8u, ca_digest, NULL, &result);
    int after = snapshot(result.after);
    result.cleanup = after == H2_PAL_OK &&
        memcmp(result.before, result.after, sizeof(result.before)) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;

    /* The suite closes every client before releasing its portable provider.
     * Compare actual native counters again, including the provider allocation. */
    runtime->mqtt = h2_pal_unsupported_mqtt_api();
    h2_coremqtt_destroy(mqtt);
    mqtt = NULL;
    memset(&mqtt_api, 0, sizeof(mqtt_api));
    size_t released[10] = {0}, expected[10];
    memcpy(expected, result.after, sizeof(expected));
    int provider_cleanup = H2_PAL_ERR_IO;
    if (after == H2_PAL_OK && expected[8] >= 1u && expected[9] >= owner_bytes) {
        --expected[8];
        expected[9] -= owner_bytes;
        if (snapshot(released) == H2_PAL_OK && memcmp(expected, released, sizeof(expected)) == 0)
            provider_cleanup = H2_PAL_OK;
    }
    if (provider_cleanup != H2_PAL_OK) result.cleanup = provider_cleanup;
    if (rc == H2_PAL_OK && result.cleanup != H2_PAL_OK) rc = result.cleanup;
    result.rc = rc;
    int confirm = rc == H2_PAL_OK ? h2_esp_h2loader_app_confirm(runtime) : H2_PAL_ERR_INVALID_STATE;
    for (;;) {
        h2_mqtt_device_replay(runtime, &result);
        printf("H2_PAL_MQTT_READY board=devkit rc=%d confirm=%d provider_cleanup=%d\n", rc, confirm, provider_cleanup);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(5000u));
    }
}
void app_main(void) {
    puts("H2_PAL_MQTT_PLATFORM_BOOT board=devkit");
    fflush(stdout);
    int rc = h2_esp_target_task_policy_install();
    if (rc != H2_PAL_OK) fail("task-policy", rc);
    h2_runtime_config_t config = {0};
    rc = h2_esp_board_runtime_config(&config);
    if (rc != H2_PAL_OK) fail("board", rc);
    size_t before[10], after[10];
    rc = snapshot(before);
    if (rc != H2_PAL_OK) fail("provider-before", rc);
    const h2_coremqtt_config_t mqtt_config = {.allocator = config.mem,
        .net = config.net, .time = config.time, .log = config.log,
        .outgoing_publish_records = 8u, .incoming_publish_records = 8u};
    rc = h2_coremqtt_create(&mqtt_config, &mqtt, &mqtt_api);
    if (rc != H2_PAL_OK) fail("mqtt-create", rc);
    rc = snapshot(after);
    if (rc != H2_PAL_OK || memcmp(before, after, 8u * sizeof(size_t)) != 0 ||
        after[8] != before[8] + 1u || after[9] <= before[9]) {
        h2_coremqtt_destroy(mqtt);
        mqtt = NULL;
        fail("provider-allocation", H2_PAL_ERR_IO);
    }
    owner_bytes = after[9] - before[9];
    config.mqtt = &mqtt_api;
    const h2_esp_h2loader_app_commands_config_t commands = {.active_name = "pal-mqtt",
        .hardware_capabilities = H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI,
        .h2loader_partition_id = 1u, .coredump_partition_id = 3u};
    rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(&config, &commands);
    if (rc == H2_PAL_OK) rc = h2_runtime_init(&config, &runtime);
    if (rc != H2_PAL_OK) {
        h2_coremqtt_destroy(mqtt);
        mqtt = NULL;
        fail("runtime-or-commands", rc);
    }
    printf("H2_PAL_MQTT_PROVIDER policy=coremqtt allocator=psram incoming=8 outgoing=8 owner_bytes=%zu\n", owner_bytes);
    const h2_pal_task_options_t options = {.name = h2_pal_mqtt_device_runner_task_name, .min_stack_size = 65536u};
    h2_pal_task_t *task = NULL;
    rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
    if (rc != H2_PAL_OK) {
        runtime->mqtt = h2_pal_unsupported_mqtt_api();
        h2_coremqtt_destroy(mqtt);
        mqtt = NULL;
        fail("runner", rc);
    }
}
