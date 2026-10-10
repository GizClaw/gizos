#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include "h2_pal_core_e2e.h"
#include "h2_pal_core_e2e_task_names.h"
#include "h2_runtime.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Test instrumentation belongs to this launcher. Every PAL operation still
 * reaches the board's real provider; missing capabilities stay BLOCKED. */
static h2_runtime_t *runtime;
static h2_pal_core_e2e_config_t config;
static h2_pal_core_e2e_result_t result;
static portMUX_TYPE observation_lock = portMUX_INITIALIZER_UNLOCKED;
static const char *current_case = "startup";
static int finished;
static size_t reported_cases;
static char task_report[512], task_stack_reports[3][128], task_fault_report[128];
static char log_output[1024];
static size_t log_length;
static vprintf_like_t previous_log;
static esp_timer_handle_t watchdog;

#define STACK_RECORDS 64u
static struct { void *address; size_t size; } stacks[STACK_RECORDS];
static int fail_after = -1;
static size_t stack_count, stack_bytes;

static void hold(void) {
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000u));
}
static void fail(const char *stage, int rc) {
    printf("H2_PAL_CORE_E2E_LAUNCHER_FAIL stage=%s rc=%d\n", stage, rc);
    fflush(stdout);
    hold();
}
static void *stack_allocate(void *user, size_t size) {
    (void)user;
    portENTER_CRITICAL(&observation_lock);
    int reject = fail_after == 0;
    if (fail_after > 0) --fail_after;
    portEXIT_CRITICAL(&observation_lock);
    if (reject) return NULL;
    void *p = h2_pal_mem_alloc(h2_esp_platform_psram_allocator(), size);
    if (p == NULL) return NULL;
    portENTER_CRITICAL(&observation_lock);
    unsigned slot;
    for (slot = 0; slot < STACK_RECORDS && stacks[slot].address != NULL; ++slot) {}
    if (slot < STACK_RECORDS) {
        stacks[slot].address = p;
        stacks[slot].size = size;
        ++stack_count;
        stack_bytes += size;
    }
    portEXIT_CRITICAL(&observation_lock);
    if (slot == STACK_RECORDS) {
        h2_pal_mem_free(h2_esp_platform_psram_allocator(), p);
        return NULL;
    }
    return p;
}
static void stack_release(void *user, void *p) {
    (void)user;
    if (p == NULL) return;
    portENTER_CRITICAL(&observation_lock);
    for (unsigned slot = 0; slot < STACK_RECORDS; ++slot) {
        if (stacks[slot].address != p) continue;
        --stack_count;
        stack_bytes -= stacks[slot].size;
        stacks[slot].address = NULL;
        stacks[slot].size = 0;
        break;
    }
    portEXIT_CRITICAL(&observation_lock);
    h2_pal_mem_free(h2_esp_platform_psram_allocator(), p);
}
static const h2_pal_mem_vtable_t stack_vtable = {
    .alloc = stack_allocate, .free = stack_release,
};
static const h2_pal_mem_api_t stack_allocator = {.vtable = &stack_vtable};

static h2_pal_result_t configure_tasks(const h2_esp_task_policy_config_t *policy) {
    h2_esp_task_policy_config_t observed = *policy;
    observed.psram_stack_allocator = &stack_allocator;
    return h2_esp_platform_task_configure(&observed);
}
static h2_pal_result_t task_fault(void *user, int after) {
    (void)user;
    if (after < -1) return H2_PAL_ERR_INVALID_ARG;
    portENTER_CRITICAL(&observation_lock);
    fail_after = after;
    portEXIT_CRITICAL(&observation_lock);
    return H2_PAL_OK;
}
static h2_pal_result_t actual_stack(void *user, size_t *out) {
    (void)user;
    TaskStatus_t status;
    vTaskGetInfo(NULL, &status, pdFALSE, eRunning);
    uintptr_t local_address = (uintptr_t)&status;
    *out = 0;
    portENTER_CRITICAL(&observation_lock);
    for (unsigned i = 0; i < STACK_RECORDS; ++i) {
        uintptr_t base = (uintptr_t)stacks[i].address;
        if (base == (uintptr_t)status.pxStackBase && local_address >= base &&
            local_address - base < stacks[i].size) {
            *out = stacks[i].size;
            break;
        }
    }
    portEXIT_CRITICAL(&observation_lock);
    return *out != 0 ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
static h2_pal_result_t resources(void *user, h2_pal_core_resources_t *out) {
    (void)user;
    h2_esp_platform_resource_stats_t native;
    h2_pal_result_t rc = h2_esp_platform_get_resource_stats(&native);
    if (rc != H2_PAL_OK) return rc;
    *out = (h2_pal_core_resources_t){
        .tasks = native.live_tasks, .task_stack_bytes = native.task_stack_bytes,
        .queues = native.live_queues, .mutexes = native.live_mutexes,
        .semaphores = native.live_semaphores, .conditions = native.live_conditions,
        .timers = native.live_timers, .firmware_infos = native.live_firmware_infos,
        .allocations = native.allocations, .allocation_bytes = native.allocation_bytes,
    };
    return H2_PAL_OK;
}
/* The management transport has its own real SDK heap allocator. Its live
 * packet buffers must not enter the Core PAL allocation baseline while the
 * host is collecting logs. Core, native Timer and Task-stack allocations keep
 * using the instrumented production PAL allocators. */
static void *transport_alloc(void *user, size_t size) {
    (void)user;
    return heap_caps_aligned_alloc(_Alignof(max_align_t), size,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void transport_free(void *user, void *ptr) {
    (void)user;
    heap_caps_free(ptr);
}
static void *transport_realloc(void *user, void *ptr, size_t size) {
    if (ptr == NULL) return transport_alloc(user, size);
    if (size == 0) { transport_free(user, ptr); return NULL; }
    size_t before = heap_caps_get_allocated_size(ptr);
    void *next = transport_alloc(user, size);
    if (next != NULL) {
        memcpy(next, ptr, before < size ? before : size);
        transport_free(user, ptr);
    }
    return next;
}
static const h2_pal_mem_vtable_t transport_memory_vtable = {
    .alloc = transport_alloc, .realloc = transport_realloc, .free = transport_free,
};
static const h2_pal_mem_api_t transport_memory = {.vtable = &transport_memory_vtable};
static h2_pal_result_t independent_clock(void *user, uint64_t *out) {
    (void)user;
    *out = (uint64_t)esp_timer_get_time();
    return H2_PAL_OK;
}
static int log_sink(const char *format, va_list args) {
    char line[768];
    va_list copy;
    va_copy(copy, args);
    int count = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    if (count > 0) {
        size_t size = (size_t)count < sizeof(line) ? (size_t)count : sizeof(line) - 1;
        portENTER_CRITICAL(&observation_lock);
        size_t room = sizeof(log_output) - 1u - log_length;
        if (size > room) size = room;
        memcpy(log_output + log_length, line, size);
        log_length += size;
        log_output[log_length] = '\0';
        portEXIT_CRITICAL(&observation_lock);
    }
    return previous_log(format, args);
}
static h2_pal_result_t observed_log(void *user, h2_pal_log_level_t level,
                                  const char *scope, const char *message) {
    (void)user;
    const char levels[] = {'D', 'I', 'W', 'E'};
    char prefix[] = "D (";
    prefix[0] = levels[level];
    portENTER_CRITICAL(&observation_lock);
    int matches = strstr(log_output, prefix) != NULL &&
                  strstr(log_output, scope) != NULL &&
                  strstr(log_output, message) != NULL;
    log_length = 0;
    log_output[0] = '\0';
    portEXIT_CRITICAL(&observation_lock);
    return matches ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
static void report_case(size_t index) {
    printf("H2_PAL_CORE_E2E case=%s status=%s result=%d\n", result.cases[index].id,
           h2_pal_core_e2e_status_name(result.cases[index].status), result.cases[index].result);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(10u));
}
static void case_begin(void *user, const char *id) {
    (void)user;
    while (reported_cases < H2_PAL_CORE_E2E_CASE_COUNT &&
           result.cases[reported_cases].status != H2_PAL_CORE_E2E_NOT_RUN)
        report_case(reported_cases++);
    portENTER_CRITICAL(&observation_lock);
    current_case = id;
    log_length = 0;
    log_output[0] = '\0';
    portEXIT_CRITICAL(&observation_lock);
    printf("H2_PAL_CORE_E2E begin=%s\n", id);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(10u));
}
static void deadline(void *user) {
    (void)user;
    portENTER_CRITICAL(&observation_lock);
    const char *id = current_case;
    int done = finished;
    portEXIT_CRITICAL(&observation_lock);
    if (!done) {
        printf("H2_PAL_CORE_E2E_WATCHDOG case=%s complete=0 qualified=0\n", id);
        fflush(stdout);
    }
}

typedef struct worker_probe { unsigned calls; size_t stack; int rc; } worker_probe_t;
static void probe_entry(void *user) {
    worker_probe_t *probe = user;
    ++probe->calls;
    probe->rc = actual_stack(NULL, &probe->stack);
}
static int probe_task(size_t minimum, worker_probe_t *probe) {
    *probe = (worker_probe_t){0};
    const h2_pal_task_options_t options = {
        .name = h2_pal_core_e2e_core_task_name, .min_stack_size = minimum,
    };
    h2_pal_task_t *task = NULL;
    int rc = h2_pal_task_start(runtime->task, &options, probe_entry, probe, &task);
    if (rc != H2_PAL_OK) return rc;
    rc = h2_pal_task_join(runtime->task, task);
    if (rc == H2_PAL_OK &&
        (probe->calls != 1 || probe->rc != H2_PAL_OK || probe->stack < minimum))
        rc = H2_PAL_ERR_INVALID_STATE;
    return rc;
}
static int task_probe(void) {
    worker_probe_t probe;
    /* Warm up native Task/libc state before taking heap evidence. */
    int rc = probe_task(4096u, &probe);
    if (rc != H2_PAL_OK) return rc;
    vTaskDelay(pdMS_TO_TICKS(20u));
    multi_heap_info_t before, after;
    heap_caps_get_info(&before, MALLOC_CAP_8BIT);
    portENTER_CRITICAL(&observation_lock);
    size_t count_before = stack_count, bytes_before = stack_bytes;
    portEXIT_CRITICAL(&observation_lock);
    UBaseType_t tasks_before = uxTaskGetNumberOfTasks();
    for (unsigned i = 0; i < 100u && rc == H2_PAL_OK; ++i) {
        rc = probe_task(4096u, &probe);
        /* Allow the USB console to drain the provider's Task READY line. */
        vTaskDelay(pdMS_TO_TICKS(5u));
    }
    const size_t sizes[] = {4096u, 16384u, 65536u};
    for (unsigned i = 0; i < 3u && rc == H2_PAL_OK; ++i) {
        rc = probe_task(sizes[i], &probe);
        snprintf(task_stack_reports[i], sizeof(task_stack_reports[i]),
                 "H2_PAL_CORE_TASK_STACK requested=%zu observed=%zu rc=%d",
                 sizes[i], probe.stack, rc);
        puts(task_stack_reports[i]);
    }
    if (rc == H2_PAL_OK) {
        (void)task_fault(NULL, 0);
        int failed_start = probe_task(4096u, &probe);
        (void)task_fault(NULL, -1);
        if (failed_start != H2_PAL_ERR_NO_MEMORY || probe.calls != 0u)
            rc = H2_PAL_ERR_INVALID_STATE;
        else rc = probe_task(4096u, &probe);
        snprintf(task_fault_report, sizeof(task_fault_report),
                 "H2_PAL_CORE_TASK_FAULT injected=%d recovery=%d", failed_start, rc);
        puts(task_fault_report);
    }
    vTaskDelay(pdMS_TO_TICKS(20u));
    heap_caps_get_info(&after, MALLOC_CAP_8BIT);
    portENTER_CRITICAL(&observation_lock);
    size_t count_after = stack_count, bytes_after = stack_bytes;
    portEXIT_CRITICAL(&observation_lock);
    UBaseType_t tasks_after = uxTaskGetNumberOfTasks();
    if (rc == H2_PAL_OK && (count_after != count_before || bytes_after != bytes_before ||
                           tasks_after != tasks_before)) rc = H2_PAL_ERR_INVALID_STATE;
    snprintf(task_report, sizeof(task_report), "H2_PAL_CORE_TASK_PROBE cycles=100 stacks_before=%zu stacks_after=%zu "
           "stack_bytes_before=%zu stack_bytes_after=%zu kernel_tasks_before=%u "
           "kernel_tasks_after=%u heap_bytes_delta=%lld heap_blocks_delta=%lld rc=%d",
           count_before, count_after, bytes_before, bytes_after,
           (unsigned)tasks_before, (unsigned)tasks_after,
           (long long)after.total_allocated_bytes - (long long)before.total_allocated_bytes,
           (long long)after.allocated_blocks - (long long)before.allocated_blocks, rc);
    puts(task_report);
    return rc;
}
static void run(void *user) {
    (void)user;
    vTaskDelay(pdMS_TO_TICKS(5000u));
    previous_log = vprintf;
    previous_log = esp_log_set_vprintf(log_sink);
    esp_log_level_set("pal-core", ESP_LOG_DEBUG);
    config = (h2_pal_core_e2e_config_t){
        .timeout_ms = 3000u,
        .queue_latest = H2_PAL_CORE_QUEUE_LATEST_REPLACE,
        .observe_monotonic_us = independent_clock,
        .observe_log = observed_log,
        .case_begin = case_begin,
        .allow_wall_set = 1,
        .firmware_version = esp_app_get_description()->version,
        .event_fixture = h2_esp_platform_system_event_api(),
        .observe_task_stack = actual_stack,
        .task_allocation_fault = task_fault,
        .observe_resources = resources,
    };
    int rc = h2_pal_core_e2e_run(runtime, &config, &result);
    (void)esp_log_set_vprintf(previous_log);
    while (reported_cases < H2_PAL_CORE_E2E_CASE_COUNT)
        report_case(reported_cases++);
    printf("H2_PAL_CORE_E2E contract=%u passed=%zu failed=%zu blocked=%zu not_run=%zu "
           "complete=%d qualified=%d cleanup=%d rc=%d\n", H2_PAL_CORE_E2E_CONTRACT_VERSION,
           result.passed, result.failed, result.blocked, result.not_run,
           result.complete, result.qualified, result.cleanup_result, rc);
    int probe = H2_PAL_ERR_UNAVAILABLE;
    if (result.retained_cleanup == NULL && result.baseline.retained_cleanup == NULL)
        probe = task_probe();
    portENTER_CRITICAL(&observation_lock);
    finished = 1;
    portEXIT_CRITICAL(&observation_lock);
    (void)esp_timer_stop(watchdog);
    (void)esp_timer_delete(watchdog);
    int confirm = h2_esp_h2loader_app_confirm(runtime);
    printf("H2_PAL_CORE_E2E_READY qualified=%d task_probe=%d confirm=%d\n",
           result.qualified, probe, confirm);
    fflush(stdout);
    /* Plain console bytes during reboot handoff are not a reliable result
     * channel. Replay the immutable ledger slowly after installation settles;
     * this never reruns cases or changes their verdicts. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000u));
        puts("H2_PAL_CORE_E2E_REPORT replay=1");
        for (size_t i = 0; i < H2_PAL_CORE_E2E_CASE_COUNT; ++i) {
            report_case(i);
            vTaskDelay(pdMS_TO_TICKS(90u));
        }
        printf("H2_PAL_CORE_E2E contract=%u passed=%zu failed=%zu blocked=%zu not_run=%zu "
               "complete=%d qualified=%d cleanup=%d rc=%d\n", H2_PAL_CORE_E2E_CONTRACT_VERSION,
               result.passed, result.failed, result.blocked, result.not_run,
               result.complete, result.qualified, result.cleanup_result, rc);
        vTaskDelay(pdMS_TO_TICKS(100u));
        for (unsigned i = 0; i < 3u; ++i) {
            if (task_stack_reports[i][0]) puts(task_stack_reports[i]);
            vTaskDelay(pdMS_TO_TICKS(100u));
        }
        if (task_fault_report[0]) puts(task_fault_report);
        vTaskDelay(pdMS_TO_TICKS(100u));
        if (task_report[0]) puts(task_report);
        vTaskDelay(pdMS_TO_TICKS(100u));
        printf("H2_PAL_CORE_E2E_READY qualified=%d task_probe=%d confirm=%d\n",
               result.qualified, probe, confirm);
        fflush(stdout);
    }
}
void app_main(void) {
    int rc = h2_esp_target_task_policy_install_with_configure(configure_tasks);
    if (rc != H2_PAL_OK) fail("task_policy", rc);
    h2_runtime_config_t runtime_config = {0};
    rc = h2_esp_board_runtime_config(&runtime_config);
    if (rc != H2_PAL_OK) fail("runtime_config", rc);
    /* The Core App owns an exclusive real event fixture. The serial command
     * service uses board APIs directly and does not require a BLE event loop. */
    runtime_config.system_event = h2_pal_unsupported_system_event_api();
    const h2_esp_h2loader_app_commands_config_t commands = {
        .active_name = "pal-core", .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
        .h2loader_partition_id = 1u, .coredump_partition_id = 3u,
    };
    h2_runtime_config_t transport_config = runtime_config;
    transport_config.mem = &transport_memory;
    rc = h2_esp_h2loader_app_commands_prepare_serial_with_config(&transport_config, &commands);
    if (rc != H2_PAL_OK) fail("command_prepare", rc);
    rc = h2_runtime_init(&runtime_config, &runtime);
    if (rc != H2_PAL_OK) fail("runtime_init", rc);
    const esp_timer_create_args_t timer = {.callback = deadline, .name = "pal-core-watchdog"};
    if (esp_timer_create(&timer, &watchdog) != ESP_OK ||
        esp_timer_start_once(watchdog, UINT64_C(120000000)) != ESP_OK)
        fail("watchdog", H2_PAL_ERR_IO);
    printf("H2_PAL_CORE_E2E_BOOT board=esp_mosaico version=%s cases=%u resources=native\n",
           esp_app_get_description()->version, (unsigned)H2_PAL_CORE_E2E_CASE_COUNT);
    const h2_pal_task_options_t options = {.name = h2_pal_core_e2e_runner_task_name};
    h2_pal_task_t *runner = NULL;
    rc = h2_pal_task_start(runtime->task, &options, run, NULL, &runner);
    if (rc != H2_PAL_OK) fail("runner", rc);
}
