#include "h2_esp_io_phase.h"

#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <assert.h>
#include <setjmp.h>
#include <string.h>

static unsigned char s_stack[16384];
static SemaphoreHandle_t s_mutex, s_request, s_done;
static TaskFunction_t s_entry;
static void *s_context;
static jmp_buf s_return_to_caller;
static int s_task, s_locked, s_internal, s_isr, s_worker;
static int64_t s_now;

int64_t esp_timer_get_time(void) { return s_now; }
int esp_ptr_internal(const void *p) { (void)p; return s_internal; }
int esp_ptr_in_iram(const void *p) { (void)p; return s_internal; }
BaseType_t xPortInIsrContext(void) { return s_isr; }
StackType_t *xTaskGetStackStart(TaskHandle_t t) {
    assert(t == NULL); return s_internal ? s_stack : NULL;
}
void *heap_caps_malloc(size_t bytes, unsigned caps) {
    assert(bytes == sizeof(s_stack));
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return s_stack;
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) {
    assert(s_mutex == NULL); s_mutex = storage; return storage;
}
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage) {
    if (s_request == NULL) s_request = storage;
    else { assert(s_done == NULL); s_done = storage; }
    return storage;
}
TaskHandle_t xTaskCreateStaticPinnedToCore(
    TaskFunction_t entry, const char *name, uint32_t bytes, void *context,
    UBaseType_t priority, StackType_t *stack, StaticTask_t *storage,
    BaseType_t core) {
    assert(!strcmp(name, "$esp/safe-call") && bytes == sizeof(s_stack));
    assert(priority == 9u && core == 0 && stack == s_stack && storage != NULL);
    s_entry = entry; s_context = context; return &s_task;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, uint32_t timeout) {
    assert(timeout == portMAX_DELAY);
    if (sem == s_mutex) {
        assert(!s_locked); s_locked = 1; s_now += 5000;
    } else if (sem == s_request) {
        assert(s_worker);
    } else {
        assert(sem == s_done && !s_worker); s_now += 1000;
    }
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    if (sem == s_mutex) {
        assert(s_locked); s_locked = 0;
    } else if (sem == s_request) {
        assert(s_locked && !s_worker); s_now += 3000;
        /* Execute the real worker iteration. The fake done signal returns
         * to this caller after the real callback/context copy, not before. */
        if (setjmp(s_return_to_caller) == 0) {
            s_worker = 1; s_entry(s_context);
            assert(0);
        }
        s_worker = 0;
    } else {
        assert(sem == s_done && s_worker); s_now += 2000;
        longjmp(s_return_to_caller, 1);
    }
    return pdTRUE;
}
static void callback(void *raw) {
    int *value = raw; ++*value; s_now += 140000;
}

int main(void) {
    int value = 1;
    h2_esp_io_phase_t phase = {.started_us = 0};
    assert(h2_esp_platform_safe_call_timed(
        callback, &value, sizeof(value), 1024u, &phase) == H2_PAL_OK);
    assert(value == 2 && !s_locked);
    assert(phase.shared_wait_us == 5000u && phase.dispatch_us == 3000u);
    assert(phase.native_us == 140000u && phase.native_max_us == 140000u);
    assert(phase.wake_copy_us == 3000u && phase.calls == 1u);
    assert(phase.direct_calls == 0u);
    h2_esp_io_phase_t total = {.started_us = 0, .scratch_wait_us = 11};
    h2_esp_io_phase_add(&total, &phase);
    h2_esp_io_phase_add(&total, &phase);
    assert(total.native_us == 280000u && total.native_max_us == 140000u);
    assert(total.calls == 2u && total.scratch_wait_us == 11u);
    total.native_us = UINT64_MAX - 1u;
    h2_esp_io_phase_add(&total, &phase);
    assert(total.native_us == UINT64_MAX);
    assert(h2_esp_io_phase_elapsed(7u, 3u) == 0u);
    assert(h2_esp_io_phase_elapsed(0u, 3u) == 3u);

    s_internal = 1;
    phase = (h2_esp_io_phase_t){0};
    assert(h2_esp_platform_safe_call_timed(
        callback, &value, sizeof(value), 1024u, &phase) == H2_PAL_OK);
    assert(value == 3 && phase.native_us == 140000u && phase.calls == 1u);
    assert(phase.shared_wait_us == 0u && phase.dispatch_us == 0u);
    assert(phase.wake_copy_us == 0u && !s_locked);
    assert(phase.direct_calls == 1u);
    s_isr = 1;
    assert(h2_esp_platform_safe_call_timed(
        callback, &value, sizeof(value), 1024u, &phase) ==
        H2_PAL_ERR_INVALID_ARG);
    assert(value == 3 && !s_locked);
    return 0;
}
