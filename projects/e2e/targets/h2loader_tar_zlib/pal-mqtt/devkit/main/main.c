#include "h2_pal_e2e_application_host.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_platform_core.h"
#include "h2_coremqtt.h"
#include "device_runner.h"
#include "ledger_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include <stdio.h>
#include <string.h>

static h2_coremqtt_t *mqtt;
static h2_pal_mqtt_api_t mqtt_api;
static h2_mqtt_device_result_t result;
static size_t owner_bytes;
static h2_mqtt_esp_ledger_console_t console;
static const h2_pal_log_api_t *board_log;

static int ledger_log(void *user,h2_pal_log_level_t level,const char *scope,const char *message) {
    (void)user;
    if(scope!=NULL && strcmp(scope,"pal-mqtt")==0)return h2_mqtt_esp_console_record(&console,message);
    return h2_pal_log_write(board_log,level,scope,message);
}
static const h2_pal_log_vtable_t ledger_log_vtable={.write=ledger_log};
static const h2_pal_log_api_t ledger_log_api={.vtable=&ledger_log_vtable};
static int confirm_app(void *user) { return h2_esp_h2loader_app_confirm(user); }

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
static h2_pal_result_t run(h2_application_host_t *host, h2_runtime_t *runtime, void *user) {
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(5000u));
    int rc;
    for (;;) {
        rc = h2_mqtt_device_prepare(runtime);
        if (rc == H2_PAL_OK) break;
        if (rc != H2_PAL_ERR_NOT_FOUND && rc != H2_PAL_ERR_UNAVAILABLE &&
            rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_BUSY) h2_pal_e2e_application_host_fail("network", rc);
        printf("H2_PAL_MQTT_SETUP_WAIT network=not_ready rc=%d cases_started=0\n", rc);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(3000u));
    }
    rc = snapshot(result.before);
    if (rc != H2_PAL_OK) h2_pal_e2e_application_host_fail("before", rc);
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
    /* One immutable terminal ledger per boot. Later replay cannot replace an
     * incomplete first capture in the independent host admission. */
    int delivered=h2_mqtt_esp_console_complete(&console,runtime,&result,provider_cleanup,confirm_app,runtime);
    if(delivered!=H2_PAL_OK)h2_pal_e2e_application_host_fail("ledger-or-confirm",delivered);
    h2_pal_result_t waited = h2_pal_e2e_application_host_wait(host, runtime);
  return rc == H2_PAL_OK ? waited : rc;
}

static h2_pal_result_t configure(void *user, h2_runtime_config_t *config) {
  (void)user;
  (void)config;
  int rc = H2_PAL_OK;
  size_t before[10], after[10];
  rc = snapshot(before);
  if (rc != H2_PAL_OK)
    h2_pal_e2e_application_host_fail("provider-before", rc);
  const h2_coremqtt_config_t mqtt_config = {.allocator = config->mem,
                                            .net = config->net,
                                            .time = config->time,
                                            .log = config->log,
                                            .outgoing_publish_records = 8u,
                                            .incoming_publish_records = 8u};
  rc = h2_coremqtt_create(&mqtt_config, &mqtt, &mqtt_api);
  if (rc != H2_PAL_OK)
    h2_pal_e2e_application_host_fail("mqtt-create", rc);
  rc = snapshot(after);
  if (rc != H2_PAL_OK || memcmp(before, after, 8u * sizeof(size_t)) != 0 ||
      after[8] != before[8] + 1u || after[9] <= before[9]) {
    h2_coremqtt_destroy(mqtt);
    mqtt = NULL;
    h2_pal_e2e_application_host_fail("provider-allocation", H2_PAL_ERR_IO);
  }
  owner_bytes = after[9] - before[9];
  config->mqtt = &mqtt_api;
  return H2_PAL_OK;
}
static h2_pal_result_t validate(void *user, h2_runtime_t *runtime) {
  (void)user;
  (void)runtime;
  board_log = runtime->log;
  console.usb = h2_esp_platform_usb_jtag_io_stream_api();
  console.timeout_ms = 5000u;
  runtime->log = &ledger_log_api;
  printf("H2_PAL_MQTT_PROVIDER policy=coremqtt allocator=psram incoming=8 "
         "outgoing=8 owner_bytes=%zu\n",
         owner_bytes);

  return H2_PAL_OK;
}
static h2_pal_result_t release_runtime(void *user) {
    (void)user;
    if (mqtt != NULL) {
        h2_coremqtt_destroy(mqtt);
        mqtt = NULL;
        memset(&mqtt_api, 0, sizeof(mqtt_api));
    }
    return H2_PAL_OK;
}
void app_main(void) {
    puts("H2_PAL_MQTT_PLATFORM_BOOT board=devkit");
    fflush(stdout);
    const h2_application_host_config_t app = {
        .entry = run, .configure = configure, .validate = validate,
        .release_runtime = release_runtime};
    (void)h2_pal_e2e_application_host_start(
        "pal-mqtt", H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI, &app);
}
