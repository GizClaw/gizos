#include "h2_esp_platform_core.h"
#include "h2_esp_resource_stats_internal.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

#define TIMER_STACK_BYTES 4096u

struct h2_pal_timer {
    TaskHandle_t worker;
    StaticTask_t worker_storage;
    StackType_t *stack;
    SemaphoreHandle_t lock, idle, done;
    StaticSemaphore_t lock_storage, idle_storage, done_storage;
    h2_pal_timer_cb_t callback;
    void *user;
    uint32_t period_ms;
    uint64_t due_us, generation;
    bool repeat, running, in_callback, stopping, destroying, self_cleanup;
    struct h2_pal_timer *reap_next;
};

/* Self-destruction cannot free its own active native stack. This fixed-storage
 * platform service reclaims it after the callback/worker has stopped. */
static portMUX_TYPE service_lock = portMUX_INITIALIZER_UNLOCKED;
static StaticTask_t reaper_storage;
static StackType_t reaper_stack[2048];
static TaskHandle_t reaper_task;
static h2_pal_timer_t *reap_head;
static int service_state;

static void dispose(h2_pal_timer_t *timer) {
    /* Match IDF's synchronous WithCaps deletion: remove the worker from every
     * core before freeing caller-provided static stack/TCB storage. */
    vTaskSuspend(timer->worker);
    for (;;) {
        bool executing = false;
        for (BaseType_t core = 0; core < CONFIG_FREERTOS_NUMBER_OF_CORES; ++core)
            if (xTaskGetCurrentTaskHandleForCore(core) == timer->worker) executing = true;
        if (!executing) break;
        taskYIELD();
    }
    vTaskDelete(timer->worker);
    vSemaphoreDelete(timer->lock);
    vSemaphoreDelete(timer->idle);
    vSemaphoreDelete(timer->done);
    h2_pal_mem_free(h2_esp_platform_internal_allocator(), timer->stack);
    h2_pal_mem_free(h2_esp_platform_internal_allocator(), timer);
    h2_esp_resource_release(H2_ESP_RESOURCE_TIMER, 0u);
}
static void reaper(void *unused) {
    (void)unused;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            portENTER_CRITICAL(&service_lock);
            h2_pal_timer_t *timer = reap_head;
            if (timer != NULL) reap_head = timer->reap_next;
            portEXIT_CRITICAL(&service_lock);
            if (timer == NULL) break;
            dispose(timer);
        }
    }
}
static int start_service(void) {
    for (;;) {
        portENTER_CRITICAL(&service_lock);
        int state = service_state;
        if (state == 0) service_state = 1;
        portEXIT_CRITICAL(&service_lock);
        if (state == 2) return H2_PAL_OK;
        if (state == 1) { vTaskDelay(1); continue; }
        TaskHandle_t created = xTaskCreateStaticPinnedToCore(
            reaper, "h2_timer_reap", sizeof(reaper_stack), NULL, tskIDLE_PRIORITY + 4u,
            reaper_stack, &reaper_storage, tskNO_AFFINITY);
        portENTER_CRITICAL(&service_lock);
        reaper_task = created;
        service_state = created != NULL ? 2 : 0;
        portEXIT_CRITICAL(&service_lock);
        return created != NULL ? H2_PAL_OK : H2_PAL_ERR_NO_MEMORY;
    }
}
static void arm(h2_pal_timer_t *timer) {
    timer->running = true;
    timer->due_us = (uint64_t)esp_timer_get_time() + (uint64_t)timer->period_ms * 1000u;
    ++timer->generation;
}
static TickType_t wait_ticks(uint64_t microseconds) {
    uint64_t ticks = (microseconds * configTICK_RATE_HZ + 999999u) / 1000000u;
    if (ticks >= portMAX_DELAY) return portMAX_DELAY - 1u;
    return ticks == 0u ? 1u : (TickType_t)ticks;
}
static void timer_worker(void *user) {
    h2_pal_timer_t *timer = user;
    for (;;) {
        (void)xSemaphoreTake(timer->lock, portMAX_DELAY);
        if (timer->destroying) {
            bool self = timer->self_cleanup;
            (void)xSemaphoreGive(timer->lock);
            (void)xSemaphoreGive(timer->done);
            if (self) {
                portENTER_CRITICAL(&service_lock);
                timer->reap_next = reap_head;
                reap_head = timer;
                portEXIT_CRITICAL(&service_lock);
                xTaskNotifyGive(reaper_task);
            }
            /* A join/reaper may now delete this stack. Touch no owner state. */
            for (;;) vTaskSuspend(NULL);
        }
        uint64_t now = (uint64_t)esp_timer_get_time();
        if (!timer->running || now < timer->due_us) {
            TickType_t ticks = timer->running ? wait_ticks(timer->due_us - now) : portMAX_DELAY;
            (void)xSemaphoreGive(timer->lock);
            (void)ulTaskNotifyTake(pdTRUE, ticks);
            continue;
        }
        uint64_t generation = timer->generation, due = timer->due_us;
        if (!timer->repeat) timer->running = false;
        timer->in_callback = true;
        (void)xSemaphoreGive(timer->lock);
        timer->callback(timer->user, timer);
        (void)xSemaphoreTake(timer->lock, portMAX_DELAY);
        timer->in_callback = false;
        if (timer->repeat && timer->running && !timer->destroying &&
            timer->generation == generation) {
            uint64_t period = (uint64_t)timer->period_ms * 1000u;
            now = (uint64_t)esp_timer_get_time();
            timer->due_us = due + ((now - due) / period + 1u) * period;
        }
        (void)xSemaphoreGive(timer->idle);
        (void)xSemaphoreGive(timer->lock);
    }
}
static h2_pal_result_t timer_create(void *user, const h2_pal_timer_config_t *config,
                                  h2_pal_timer_t **out) {
    (void)user;
    if (out != NULL) *out = NULL;
    if (config == NULL || out == NULL || config->cb == NULL || config->period_ms == 0 ||
        (config->flags & ~(H2_PAL_TIMER_FLAG_AUTO_START | H2_PAL_TIMER_FLAG_REPEAT)) != 0)
        return H2_PAL_ERR_INVALID_ARG;
    int rc = start_service();
    if (rc != H2_PAL_OK) return rc;
    const h2_pal_mem_api_t *memory = h2_esp_platform_internal_allocator();
    h2_pal_timer_t *timer = h2_pal_mem_alloc(memory, sizeof(*timer));
    if (timer == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(timer, 0, sizeof(*timer));
    timer->stack = h2_pal_mem_alloc(memory, TIMER_STACK_BYTES);
    if (timer->stack == NULL) { h2_pal_mem_free(memory, timer); return H2_PAL_ERR_NO_MEMORY; }
    timer->lock = xSemaphoreCreateMutexStatic(&timer->lock_storage);
    timer->idle = xSemaphoreCreateBinaryStatic(&timer->idle_storage);
    timer->done = xSemaphoreCreateBinaryStatic(&timer->done_storage);
    timer->callback = config->cb;
    timer->user = config->cb_user;
    timer->period_ms = config->period_ms;
    timer->repeat = (config->flags & H2_PAL_TIMER_FLAG_REPEAT) != 0;
    (void)xSemaphoreTake(timer->lock, portMAX_DELAY);
    timer->worker = xTaskCreateStaticPinnedToCore(
        timer_worker, config->name != NULL ? config->name : "h2_timer", TIMER_STACK_BYTES,
        timer, tskIDLE_PRIORITY + 4u, timer->stack, &timer->worker_storage, tskNO_AFFINITY);
    if (timer->worker == NULL) {
        (void)xSemaphoreGive(timer->lock);
        vSemaphoreDelete(timer->lock); vSemaphoreDelete(timer->idle); vSemaphoreDelete(timer->done);
        h2_pal_mem_free(memory, timer->stack); h2_pal_mem_free(memory, timer);
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_esp_resource_acquire(H2_ESP_RESOURCE_TIMER, 0u);
    *out = timer;
    if ((config->flags & H2_PAL_TIMER_FLAG_AUTO_START) != 0) arm(timer);
    (void)xSemaphoreGive(timer->lock);
    return H2_PAL_OK;
}
static h2_pal_result_t timer_destroy(void *user, h2_pal_timer_t *timer) {
    (void)user;
    if (timer == NULL) return H2_PAL_ERR_INVALID_ARG;
    (void)xSemaphoreTake(timer->lock, portMAX_DELAY);
    if (timer->stopping || timer->destroying) {
        (void)xSemaphoreGive(timer->lock);
        return H2_PAL_ERR_BUSY;
    }
    bool self = xTaskGetCurrentTaskHandle() == timer->worker;
    timer->destroying = true;
    timer->self_cleanup = self;
    timer->running = false;
    ++timer->generation;
    xTaskNotifyGive(timer->worker);
    (void)xSemaphoreGive(timer->lock);
    if (self) return H2_PAL_OK;
    (void)xSemaphoreTake(timer->done, portMAX_DELAY);
    dispose(timer);
    return H2_PAL_OK;
}
static h2_pal_result_t timer_start(void *user, h2_pal_timer_t *timer) {
    (void)user;
    if (timer == NULL) return H2_PAL_ERR_INVALID_ARG;
    (void)xSemaphoreTake(timer->lock, portMAX_DELAY);
    int rc = timer->destroying || timer->stopping ? H2_PAL_ERR_BUSY : H2_PAL_OK;
    if (rc == H2_PAL_OK) { arm(timer); xTaskNotifyGive(timer->worker); }
    (void)xSemaphoreGive(timer->lock);
    return rc;
}
static h2_pal_result_t timer_stop(void *user, h2_pal_timer_t *timer) {
    (void)user;
    if (timer == NULL) return H2_PAL_ERR_INVALID_ARG;
    (void)xSemaphoreTake(timer->lock, portMAX_DELAY);
    if (timer->destroying || timer->stopping) {
        (void)xSemaphoreGive(timer->lock);
        return H2_PAL_ERR_BUSY;
    }
    timer->running = false;
    ++timer->generation;
    xTaskNotifyGive(timer->worker);
    if (xTaskGetCurrentTaskHandle() != timer->worker) {
        timer->stopping = true;
        while (timer->in_callback) {
            (void)xSemaphoreGive(timer->lock);
            (void)xSemaphoreTake(timer->idle, portMAX_DELAY);
            (void)xSemaphoreTake(timer->lock, portMAX_DELAY);
        }
        timer->stopping = false;
    }
    (void)xSemaphoreGive(timer->lock);
    return H2_PAL_OK;
}
static h2_pal_result_t timer_period(void *user, h2_pal_timer_t *timer, uint32_t ms) {
    (void)user;
    if (timer == NULL || ms == 0) return H2_PAL_ERR_INVALID_ARG;
    (void)xSemaphoreTake(timer->lock, portMAX_DELAY);
    int rc = timer->destroying || timer->stopping ? H2_PAL_ERR_BUSY : H2_PAL_OK;
    if (rc == H2_PAL_OK) {
        timer->period_ms = ms;
        if (timer->running) { arm(timer); xTaskNotifyGive(timer->worker); }
    }
    (void)xSemaphoreGive(timer->lock);
    return rc;
}
static h2_pal_result_t timer_running(void *user, h2_pal_timer_t *timer, int *out) {
    (void)user;
    if (timer == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
    (void)xSemaphoreTake(timer->lock, portMAX_DELAY);
    *out = timer->running;
    (void)xSemaphoreGive(timer->lock);
    return H2_PAL_OK;
}
const h2_pal_timer_api_t *h2_esp_platform_timer_api(void) {
    static const h2_pal_timer_vtable_t vtable = {
        .create = timer_create, .destroy = timer_destroy, .start = timer_start,
        .stop = timer_stop, .reset = timer_start, .set_period_ms = timer_period,
        .is_running = timer_running,
    };
    static const h2_pal_timer_api_t api = {.vtable = &vtable};
    (void)start_service();
    return &api;
}
