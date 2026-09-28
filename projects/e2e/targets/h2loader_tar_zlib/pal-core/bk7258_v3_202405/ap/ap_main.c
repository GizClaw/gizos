#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_loader_app_client.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include "h2_pal_core_e2e.h"
#include "h2_pal_core_e2e_task_names.h"
#include "bk_private/bk_init.h"
#include <driver/aon_rtc.h>
#include <os/os.h>
#include <os/mem.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "h2_atomic.h"
#include "h2_bk_task_lifetime_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static h2_runtime_t *runtime;
static h2_runtime_t transport_runtime;
static h2_pal_core_e2e_result_t result;
static int fail_after = -1, finished, commands_ready;
static size_t reported;
static const char *current_case = "startup";
static char last_log[1024], last_scope[64];
static int last_level;
static char atomic_report[192];
static struct { const char *id; h2_pal_core_resources_t value; } resource_samples[16];
static size_t resource_sample_count;
static void hold(void) { for (;;) rtos_delay_milliseconds(1000); }
static void fail(const char *stage, int rc) {
    printf("H2_BK_CORE_LAUNCHER_FAIL stage=%s rc=%d\n", stage, rc);
    if (!commands_ready) {
        rtos_delay_milliseconds(200);
        /* Preserve the consumed, unconfirmed handoff record. Explicitly
         * selecting P1 clears rollback evidence and can retry the same Stage. */
        (void)h2_pal_power_reboot(h2_bk_h2loader_power_api(), 0u);
    }
    hold();
}
static void *stack_alloc(void *user, size_t size) {
    (void)user;
    uint32_t level = rtos_enter_critical();
    int reject = fail_after == 0;
    if (fail_after > 0) --fail_after;
    rtos_exit_critical(level);
    return reject ? NULL : h2_pal_mem_alloc(h2_bk_platform_psram_allocator(), size);
}
static void stack_free(void *user, void *ptr) {
    (void)user;
    h2_pal_mem_free(h2_bk_platform_psram_allocator(), ptr);
}
static const h2_pal_mem_vtable_t stack_methods = {.alloc = stack_alloc, .free = stack_free};
static const h2_pal_mem_api_t stack_memory = {.vtable = &stack_methods};
extern h2_pal_result_t __real_h2_bk_platform_task_configure(const h2_bk_task_policy_config_t *config);
h2_pal_result_t __wrap_h2_bk_platform_task_configure(const h2_bk_task_policy_config_t *config) {
    h2_bk_task_policy_config_t observed = *config;
    observed.psram_stack_allocator = &stack_memory;
    return __real_h2_bk_platform_task_configure(&observed);
}
static h2_pal_result_t task_fault(void *user, int after) {
    (void)user;
    if (after < -1) return H2_PAL_ERR_INVALID_ARG;
    uint32_t level = rtos_enter_critical();
    fail_after = after;
    rtos_exit_critical(level);
    return H2_PAL_OK;
}
static h2_pal_result_t actual_stack(void *user, size_t *out) {
    (void)user;
    uint32_t top, bottom, size;
    if (xTaskGetStackInfo(xTaskGetCurrentTaskHandle(), &top, &bottom, &size) != pdTRUE)
        return H2_PAL_ERR_INVALID_STATE;
    uintptr_t address = (uintptr_t)&top;
    if (address < bottom - size || address >= bottom) return H2_PAL_ERR_INVALID_STATE;
    *out = size;
    printf("H2_BK_CORE_STACK observed=%u local_in_stack=1\n", (unsigned)size);
    return H2_PAL_OK;
}
static h2_pal_result_t resources(void *user, h2_pal_core_resources_t *out) {
    (void)user;
    h2_bk_platform_resource_stats_t v;
    h2_pal_result_t rc = h2_bk_platform_get_resource_stats(&v);
    if (rc != H2_PAL_OK) return rc;
    *out = (h2_pal_core_resources_t){.tasks=v.live_tasks,.task_stack_bytes=v.task_stack_bytes,
        .queues=v.live_queues,.mutexes=v.live_mutexes,.semaphores=v.live_semaphores,
        .conditions=v.live_conditions,.timers=v.live_timers,.firmware_infos=v.live_firmware_infos,
        .allocations=v.allocations,.allocation_bytes=v.allocation_bytes};
    if (resource_sample_count < 16u &&
        (strcmp(current_case,"startup")==0 || strcmp(current_case,"pal.core.timer.churn")==0 ||
         strcmp(current_case,"pal.core.resources.recovered")==0) &&
        (resource_sample_count==0 || resource_samples[resource_sample_count-1].id!=current_case ||
         memcmp(&resource_samples[resource_sample_count-1].value,out,sizeof(*out))!=0)) {
        resource_samples[resource_sample_count].id=current_case;
        resource_samples[resource_sample_count++].value=*out;
    }
    return H2_PAL_OK;
}
static void report_resources(void) {
    for (size_t i=0;i<resource_sample_count;++i) {
        const h2_pal_core_resources_t *v=&resource_samples[i].value;
        printf("H2_BK_CORE_RESOURCES case=%s tasks=%u stacks=%u queues=%u mutexes=%u semaphores=%u conditions=%u timers=%u infos=%u allocations=%u bytes=%u\n",
            resource_samples[i].id,(unsigned)v->tasks,(unsigned)v->task_stack_bytes,
            (unsigned)v->queues,(unsigned)v->mutexes,(unsigned)v->semaphores,
            (unsigned)v->conditions,(unsigned)v->timers,(unsigned)v->firmware_infos,
            (unsigned)v->allocations,(unsigned)v->allocation_bytes);
        rtos_delay_milliseconds(100);
    }
}
static h2_pal_result_t clock_us(void *user, uint64_t *out) {
    (void)user; *out = bk_aon_rtc_get_us(); return H2_PAL_OK;
}
extern void __real_bk_printf_ext(int level, char *tag, const char *format, ...);
void __wrap_bk_printf_ext(int level, char *tag, const char *format, ...) {
    char text[1024];
    va_list args; va_start(args, format); vsnprintf(text, sizeof(text), format, args); va_end(args);
    if (tag != NULL && strcmp(tag, "pal-core") == 0) {
        snprintf(last_log, sizeof(last_log), "%s", text);
        snprintf(last_scope, sizeof(last_scope), "%s", tag);
        last_level = level;
    }
    __real_bk_printf_ext(level, tag, "%s", text);
}
static h2_pal_result_t log_seen(void *user, h2_pal_log_level_t level,
                               const char *scope, const char *message) {
    (void)user;
    const int sdk_levels[] = {4,3,2,1};
    int match = last_level == sdk_levels[level] && strcmp(scope,last_scope) == 0 &&
                strncmp(last_log,message,strlen(message)) == 0;
    last_log[0] = 0; last_scope[0] = 0;
    return match ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
static void report_case(size_t i) {
    printf("H2_BK_CORE_CASE id=%s status=%s rc=%d\n", result.cases[i].id,
           h2_pal_core_e2e_status_name(result.cases[i].status), result.cases[i].result);
    rtos_delay_milliseconds(100);
}
static void begin(void *user, const char *id) {
    (void)user;
    while (reported < H2_PAL_CORE_E2E_CASE_COUNT &&
           result.cases[reported].status != H2_PAL_CORE_E2E_NOT_RUN) report_case(reported++);
    current_case = id;
    printf("H2_BK_CORE_BEGIN id=%s\n", id);
}
static void watchdog(void *user) {
    (void)user;
    rtos_delay_milliseconds(120000);
    if (!finished) printf("H2_BK_CORE_WATCHDOG case=%s\n",current_case);
    hold();
}
/* Management traffic uses its own SDK allocator; Core's actual PAL heap
 * accounting cannot be perturbed by asynchronous UART packet buffers. */
static void *transport_alloc(void *u,size_t n) {(void)u;return psram_malloc(n);}
static void *transport_realloc(void *u,void *p,size_t n) {(void)u;return psram_realloc(p,n);}
static void transport_free(void *u,void *p) {(void)u;os_free(p);}
static const h2_pal_mem_vtable_t transport_methods = {
    .alloc=transport_alloc,.realloc=transport_realloc,.free=transport_free};
static const h2_pal_mem_api_t transport_memory = {.vtable=&transport_methods};
/* BK's service defaults its command/package allocator to native PAL PSRAM.
 * Select the same real SDK PSRAM heap for the management fixture before client
 * initialization, including its embedded Loader. Status metadata reads must
 * not enter the independent Core allocation baseline. */
extern int __real_h2_loader_app_client_init(h2_loader_app_client_t *,
                                           const h2_loader_app_client_config_t *);
int __wrap_h2_loader_app_client_init(h2_loader_app_client_t *client,
                                    const h2_loader_app_client_config_t *config) {
    h2_loader_app_client_config_t isolated=*config;
    isolated.allocator=&transport_memory;
    return __real_h2_loader_app_client_init(client,&isolated);
}

static void run(void *user) {
    (void)user;
    rtos_delay_milliseconds(5000);
    int rc;
    h2_pal_time_wall_status_t wall_status;
    rc=h2_pal_time_get_wall_status(runtime->time,&wall_status);
    if(rc!=H2_PAL_OK) fail("clock_status",rc);
    if(!wall_status.valid) {
        uint64_t wall=1;
        rc=h2_pal_time_get_wall_ms(runtime->time,&wall);
        if(rc!=H2_PAL_TIME_ERR_UNCALIBRATED || wall!=0) fail("clock_uncalibrated",rc);
        /* Seed an uncalibrated real RTC from image build UTC so the set/restore
         * case has a valid baseline. This is not a replacement clock. */
        rc=h2_pal_time_set_wall_ms(runtime->time,H2_BK_CORE_EPOCH_MS);
        if(rc!=H2_PAL_OK) fail("clock_calibration",rc);
        printf("H2_BK_CORE_CLOCK boot_valid=0 uncalibrated_read=PASS calibrated_from=build_utc\n");
    }
    printf("H2_BK_CORE_BOOT version=%s cases=%u\n",H2_BK_CORE_VERSION,(unsigned)H2_PAL_CORE_E2E_CASE_COUNT);
    const h2_pal_core_e2e_config_t config={.timeout_ms=3000,
        .queue_latest=H2_PAL_CORE_QUEUE_LATEST_REPLACE,.allow_wall_set=1,
        .firmware_version=H2_BK_CORE_VERSION,.observe_monotonic_us=clock_us,.observe_log=log_seen,
        .case_begin=begin,.event_fixture=h2_bk_platform_system_event_api(),
        .observe_resources=resources,.observe_task_stack=actual_stack,.task_allocation_fault=task_fault};
    rc=h2_pal_core_e2e_run(runtime,&config,&result);
    finished=1;
    while(reported<H2_PAL_CORE_E2E_CASE_COUNT) report_case(reported++);
    int confirm=h2_bk_h2loader_confirm_current_app(&transport_runtime);
    for (;;) {
        printf("H2_BK_CORE_REPORT contract=%u passed=%u failed=%u blocked=%u not_run=%u complete=%d qualified=%d cleanup=%d rc=%d confirm=%d\n",
            H2_PAL_CORE_E2E_CONTRACT_VERSION,(unsigned)result.passed,(unsigned)result.failed,
            (unsigned)result.blocked,(unsigned)result.not_run,result.complete,result.qualified,
            result.cleanup_result,rc,confirm);
        rtos_delay_milliseconds(100);
        puts(atomic_report);
        report_resources();
        rtos_delay_milliseconds(5000);
        for(size_t i=0;i<H2_PAL_CORE_E2E_CASE_COUNT;++i) report_case(i);
    }
}
/* Runtime depends on Atomic. Verify its real cross-core prerequisite before
 * entering the separate PAL Core contract; this is not a PAL case. */
static h2_atomic_uint_t atomic_count, atomic_ready;
static SemaphoreHandle_t atomic_done;
static unsigned observed_cores[2];
static int atomic_timeout[2];
static void atomic_worker(void *user) {
    unsigned index = (unsigned)(uintptr_t)user;
    observed_cores[index] = (unsigned)rtos_get_core_id();
    h2_atomic_uint_fetch_add(&atomic_ready,1,H2_ATOMIC_SEQ_CST);
    uint64_t deadline_us = bk_aon_rtc_get_us() + 3000000u;
    while (h2_atomic_uint_load(&atomic_ready,H2_ATOMIC_SEQ_CST) != 2u) {
        if (bk_aon_rtc_get_us() > deadline_us) { atomic_timeout[index]=1; break; }
    }
    if (!atomic_timeout[index])
        for (unsigned i=0;i<50000u;++i)
            h2_atomic_uint_fetch_add(&atomic_count,1,H2_ATOMIC_SEQ_CST);
    xSemaphoreGive(atomic_done);
    for (;;) vTaskSuspend(NULL);
}
static void atomic_prerequisite(void) {
    if (h2_atomic_uint_init(&atomic_count,0)!=H2_ATOMIC_OK ||
        h2_atomic_uint_init(&atomic_ready,0)!=H2_ATOMIC_OK) fail("atomic_init",H2_PAL_ERR_UNSUPPORTED);
    StaticSemaphore_t storage = {0};
    atomic_done=xSemaphoreCreateCountingStatic(2,0,&storage);
    beken_thread_t workers[2];
    if (atomic_done == NULL ||
        rtos_core0_create_psram_thread(&workers[0],7,"core_atomic0",atomic_worker,4096,(void *)0)!=kNoErr ||
        rtos_core1_create_psram_thread(&workers[1],7,"core_atomic1",atomic_worker,4096,(void *)1)!=kNoErr)
        fail("atomic_threads",H2_PAL_ERR_TASK);
    for(unsigned i=0;i<2u;++i)
        if(xSemaphoreTake(atomic_done,5000u)!=pdTRUE) fail("atomic_wait",H2_PAL_ERR_TIMEOUT);
    for(unsigned i=0;i<2u;++i) h2_bk_delete_stopped_task(workers[i]);
    unsigned count=h2_atomic_uint_load(&atomic_count,H2_ATOMIC_SEQ_CST);
    /* rtos_get_core_id reports chip IDs: CP is 0, AP workers are 1/2.
     * The SDK affinity arguments above are AP-local 0/1. */
    int passed=count==100000u && observed_cores[0]==(unsigned)CPU_ID_OFFSET &&
               observed_cores[1]==(unsigned)CPU_ID_OFFSET+1u &&
               !atomic_timeout[0] && !atomic_timeout[1];
    snprintf(atomic_report,sizeof(atomic_report),
             "H2_BK_CORE_ATOMIC_PREREQUISITE physical_cores=%u,%u expected=100000 observed=%u no_yield=1 pass=%d",
             observed_cores[0],observed_cores[1],count,passed);
    puts(atomic_report);
    vSemaphoreDelete(atomic_done);
    h2_atomic_uint_destroy(&atomic_ready); h2_atomic_uint_destroy(&atomic_count);
    if(!passed) fail("atomic_parallel",H2_PAL_ERR_INVALID_STATE);
}
static void entry(void *user) {
    (void)user;
    atomic_prerequisite();
    h2_runtime_config_t config;
    int rc=h2_bk7258_board_runtime_config(&config);
    if(rc!=H2_PAL_OK) fail("board",rc);
    config.system_event=h2_pal_unsupported_system_event_api();
    rc=h2_runtime_init(&config,&runtime);
    if(rc!=H2_PAL_OK) fail("runtime",rc);
    transport_runtime=*runtime; transport_runtime.mem=&transport_memory;
    rc=h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(&transport_runtime,"pal-core",H2_LOADER_CAPABILITY_UART);
    if(rc!=H2_PAL_OK) fail("commands",rc);
    commands_ready=1;
    beken_thread_t deadline;
    if(rtos_core0_create_psram_thread(&deadline,7,"core_watchdog",watchdog,4096,NULL)!=kNoErr)
        fail("watchdog",H2_PAL_ERR_TASK);
    const h2_pal_task_options_t options={.name=h2_pal_core_e2e_runner_task_name};
    h2_pal_task_t *runner;
    rc=h2_pal_task_start(runtime->task,&options,run,NULL,&runner);
    if(rc!=H2_PAL_OK) fail("runner",rc);
    hold();
}
int main(void) {
    if(h2_bk_target_task_policy_install()!=H2_PAL_OK) return -1;
    bk_init();
    int rc=h2_bk7258_board_start_entry_task("bk/pal-core",entry,NULL);
    if(rc!=H2_PAL_OK) fail("entry",rc);
    return 0;
}
