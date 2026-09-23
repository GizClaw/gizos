#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_target_task_policy.h"
#include "h2_pal_atomic_e2e.h"

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdio.h>

#define H2_ATOMIC_COUNT_ITERATIONS 1000000u
#define H2_ATOMIC_CONTROL_ITERATIONS 5000000u
#define H2_ATOMIC_CAS_ITERATIONS 50000u
#define H2_ATOMIC_LOCK_ITERATIONS 20000u

typedef struct h2_atomic_task_context {
    void (*entry)(void *);
    void *argument;
    TaskHandle_t parent;
    BaseType_t core;
    bool core_ok;
} h2_atomic_task_context_t;

static void h2_atomic_task(void *argument) {
    h2_atomic_task_context_t *context = argument;
    context->core_ok = xPortGetCoreID() == context->core;
    if (context->core_ok) context->entry(context->argument);
    xTaskNotifyGive(context->parent);
    vTaskDelete(NULL);
}

static h2_pal_result_t h2_atomic_run_pair(
    void *user, void (*entry)(void *), void *first, void *second) {
    (void)user;
    h2_atomic_task_context_t contexts[2] = {
        { .entry = entry, .argument = first, .parent = xTaskGetCurrentTaskHandle(), .core = 0 },
        { .entry = entry, .argument = second, .parent = xTaskGetCurrentTaskHandle(), .core = 1 },
    };
    unsigned started = 0;
    if (xTaskCreatePinnedToCore(h2_atomic_task, "pal/atomic/0", 4096,
            &contexts[0], 4, NULL, 0) == pdPASS) {
        started = 1;
        if (xTaskCreatePinnedToCore(h2_atomic_task, "pal/atomic/1", 4096,
                &contexts[1], 4, NULL, 1) == pdPASS) {
            started = 2;
        }
    }
    for (unsigned i = 0; i < started; ++i) {
        if (ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(120000)) == 0) {
            printf("H2_PAL_ATOMIC_E2E_FAIL stage=pair_timeout completed=%u started=%u\n",
                i, started);
            fflush(stdout);
            esp_restart();
            for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (started != 2) return H2_PAL_ERR_NO_MEMORY;
    return contexts[0].core_ok && contexts[1].core_ok ? H2_PAL_OK : H2_PAL_ERR_TASK;
}

static void h2_atomic_yield(void *user) {
    (void)user;
    vTaskDelay(1);
}

static void h2_atomic_hold(void) {
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

static void h2_atomic_entry(void *user) {
    (void)user;
    h2_runtime_config_t runtime_config = {0};
    h2_runtime_t *runtime = NULL;
    h2_pal_result_t rc = h2_esp_board_runtime_config(&runtime_config);
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_prepare_serial(&runtime_config,
            "pal-atomic", 1u, 3u);
    }
    if (rc == H2_PAL_OK) rc = h2_runtime_init(&runtime_config, &runtime);
    if (rc == H2_PAL_OK) {
        rc = h2_esp_h2loader_app_commands_start(runtime, "pal-atomic", 1u, 3u);
    }
    if (rc != H2_PAL_OK) {
        printf("H2_PAL_ATOMIC_E2E_FAIL stage=runtime rc=%d\n", rc);
        fflush(stdout);
        h2_atomic_hold();
    }

    const h2_pal_atomic_api_t *atomic = h2_esp_board_atomic_api();
    h2_pal_atomic_u32_t *psram_count = heap_caps_malloc(sizeof(*psram_count),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    h2_pal_atomic_u32_t *internal_count = heap_caps_malloc(sizeof(*internal_count),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    h2_pal_atomic_flag_t *psram_flag = heap_caps_malloc(sizeof(*psram_flag),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _Atomic uint32_t *raw_count = heap_caps_malloc(sizeof(*raw_count),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (atomic == NULL || psram_count == NULL || internal_count == NULL ||
        psram_flag == NULL || raw_count == NULL ||
        !esp_ptr_external_ram(psram_count) || !esp_ptr_external_ram(psram_flag) ||
        !esp_ptr_external_ram(raw_count) || !esp_ptr_internal(internal_count)) {
        printf("H2_PAL_ATOMIC_E2E_FAIL stage=allocation\n");
        fflush(stdout);
        h2_atomic_hold();
    }
    atomic_init(raw_count, 0);
    h2_pal_atomic_e2e_config_t config = {
        .atomic = atomic, .counter = psram_count, .flag = psram_flag,
        .run_pair = h2_atomic_run_pair, .yield = h2_atomic_yield,
    };
    const struct {
        const char *name;
        h2_pal_atomic_e2e_case_t test_case;
        uint32_t iterations;
        bool internal;
    } cases[] = {
        { "psram_fetch_add", H2_PAL_ATOMIC_E2E_FETCH_ADD, H2_ATOMIC_COUNT_ITERATIONS, false },
        { "internal_fetch_add", H2_PAL_ATOMIC_E2E_FETCH_ADD, H2_ATOMIC_COUNT_ITERATIONS, true },
        { "psram_cas", H2_PAL_ATOMIC_E2E_CAS, H2_ATOMIC_CAS_ITERATIONS, false },
        { "psram_exchange", H2_PAL_ATOMIC_E2E_EXCHANGE, H2_ATOMIC_LOCK_ITERATIONS, false },
        { "psram_flag", H2_PAL_ATOMIC_E2E_FLAG, H2_ATOMIC_LOCK_ITERATIONS, false },
    };
    unsigned failures = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        config.counter = cases[i].internal ? internal_count : psram_count;
        uint32_t actual = 0;
        printf("H2_PAL_ATOMIC_E2E_BEGIN case=%s iterations_per_core=%u\n",
            cases[i].name, (unsigned)cases[i].iterations);
        fflush(stdout);
        rc = h2_pal_atomic_e2e_run_case(&config, cases[i].test_case,
            cases[i].iterations, &actual);
        printf("H2_PAL_ATOMIC_E2E_CASE case=%s expected=%u actual=%u rc=%d status=%s\n",
            cases[i].name, (unsigned)(cases[i].iterations * 2),
            (unsigned)actual, rc, rc == H2_PAL_OK ? "PASS" : "FAIL");
        fflush(stdout);
        if (rc != H2_PAL_OK) ++failures;
    }
    h2_pal_atomic_e2e_c11_control_t raw = {
        .counter = raw_count, .iterations = H2_ATOMIC_CONTROL_ITERATIONS,
        .yield = h2_atomic_yield,
    };
    atomic_store_explicit(raw_count, 0, memory_order_relaxed);
    printf("H2_PAL_ATOMIC_E2E_BEGIN case=direct_c11_psram iterations_per_core=%u\n",
        H2_ATOMIC_CONTROL_ITERATIONS);
    fflush(stdout);
    rc = h2_atomic_run_pair(NULL, h2_pal_atomic_e2e_direct_c11_worker,
        &raw, &raw);
    uint32_t direct_actual = atomic_load_explicit(raw_count, memory_order_relaxed);
    uint32_t direct_lost = direct_actual <= H2_ATOMIC_CONTROL_ITERATIONS * 2
        ? H2_ATOMIC_CONTROL_ITERATIONS * 2 - direct_actual : 0;
    printf("H2_PAL_ATOMIC_E2E_CONTROL case=direct_c11_psram expected=%u actual=%u lost=%u rc=%d status=OBSERVED\n",
        H2_ATOMIC_CONTROL_ITERATIONS * 2, (unsigned)direct_actual,
        (unsigned)direct_lost, rc);
    fflush(stdout);
    if (rc != H2_PAL_OK) ++failures;

    if (failures == 0) {
        rc = h2_esp_h2loader_app_confirm(runtime);
        if (rc != H2_PAL_OK) ++failures;
    }
    printf("H2_PAL_ATOMIC_E2E_SUMMARY cases=%u failed=%u control_lost=%u status=%s\n",
        (unsigned)(sizeof(cases) / sizeof(cases[0])), failures,
        (unsigned)direct_lost, failures == 0 ? "PASS" : "FAIL");
    fflush(stdout);
    h2_atomic_hold();
}

void app_main(void) {
    if (h2_esp_target_task_policy_install() != H2_PAL_OK) return;
    int rc = h2_esp_board_start_entry_task("devkit/pal-atomic", h2_atomic_entry, NULL);
    if (rc != H2_PAL_OK) {
        printf("H2_PAL_ATOMIC_E2E_FAIL stage=entry rc=%d\n", rc);
        fflush(stdout);
    }
}
