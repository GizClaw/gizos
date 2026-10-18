#include "h2_esp_platform_safe_call.h"

#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

static unsigned char s_stack[16384], s_caller_stack[8192];
static SemaphoreHandle_t s_mutex, s_scratch_mutex, s_request, s_done;
static TaskFunction_t s_entry;
static void *s_worker_context;
static jmp_buf s_return_to_caller;
static int s_task, s_locked, s_scratch_held, s_worker, s_internal = 1;
static int s_isr, s_iram = 1, s_fail_mutex, s_fail_binary, s_fail_alloc;
static int s_fail_task, s_fail_lock, s_fail_send;
static unsigned s_allocations, s_creations, s_requests, s_callbacks;
static const uint8_t *s_external;
static size_t s_external_size;
static void *s_expected_caller;
static int s_expect_worker;

void *heap_caps_malloc(size_t size, unsigned int caps) {
    assert(size == sizeof(s_stack));
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ++s_allocations;
    return s_fail_alloc ? NULL : s_stack;
}
int esp_ptr_internal(const void *pointer) {
    const uintptr_t address = (uintptr_t)pointer;
    return pointer != NULL && !(s_external != NULL &&
        address >= (uintptr_t)s_external &&
        address - (uintptr_t)s_external < s_external_size);
}
int esp_ptr_in_iram(const void *pointer) { (void)pointer; return s_iram; }
BaseType_t xPortInIsrContext(void) { return s_isr; }
StackType_t *xTaskGetStackStart(TaskHandle_t task) {
    assert(task == NULL);
    return s_worker ? s_stack : s_internal ? s_caller_stack : NULL;
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) {
    if (s_fail_mutex) return NULL;
    if (s_mutex == NULL) s_mutex = storage;
    else { assert(s_scratch_mutex == NULL); s_scratch_mutex = storage; }
    return storage;
}
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage) {
    if (s_request == NULL) s_request = storage;
    else if (s_request != storage) s_done = storage;
    return s_fail_binary ? NULL : storage;
}
TaskHandle_t xTaskCreateStaticPinnedToCore(
    TaskFunction_t entry, const char *name, uint32_t size, void *context,
    UBaseType_t priority, StackType_t *stack, StaticTask_t *storage,
    BaseType_t core) {
    assert(entry && !strcmp(name, "$esp/safe-call") && storage);
    assert(size == 16384u && priority == 9u && core == 0 && stack == s_stack);
    ++s_creations;
    if (s_fail_task) return NULL;
    s_entry = entry; s_worker_context = context;
    return &s_task;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout) {
    assert(timeout == portMAX_DELAY);
    if (semaphore == s_mutex) {
        assert(!s_worker && !s_locked);
        if (s_fail_lock) return pdFALSE;
        s_locked = 1;
    } else if (semaphore == s_scratch_mutex) {
        assert(!s_worker && !s_scratch_held && !s_locked);
        s_scratch_held = 1;
    } else if (semaphore == s_request) {
        assert(s_worker && s_locked);
    } else {
        assert(semaphore == s_done && !s_worker && s_locked);
    }
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    if (semaphore == s_mutex) {
        assert(!s_worker && s_locked); s_locked = 0;
    } else if (semaphore == s_scratch_mutex) {
        assert(!s_worker && s_scratch_held && !s_locked); s_scratch_held = 0;
    } else if (semaphore == s_request) {
        assert(!s_worker && s_locked);
        if (s_fail_send) return pdFALSE;
        ++s_requests;
        /* Execute the production worker loop, callback and copy-back. */
        if (setjmp(s_return_to_caller) == 0) {
            s_worker = 1; s_entry(s_worker_context);
            assert(0);
        }
        s_worker = 0;
    } else {
        assert(semaphore == s_done && s_worker && s_locked);
        longjmp(s_return_to_caller, 1);
    }
    return pdTRUE;
}

typedef struct callback_context {
    int value;
    int nested;
    unsigned char bytes[32];
} callback_context_t;
static void increment(void *raw) { ++*(int *)raw; }
static void callback(void *raw) {
    callback_context_t *context = raw;
    ++s_callbacks;
    assert(s_worker == s_expect_worker);
    assert(s_worker ? raw != s_expected_caller : raw == s_expected_caller);
    ++context->value;
    context->bytes[31] = 0x5a;
    if (context->nested) {
        const unsigned requests = s_requests;
        int nested = 40;
        assert(s_worker && s_locked);
        assert(h2_esp_platform_safe_call_on_worker(
            increment, &nested, sizeof(nested), 1024u) == H2_PAL_OK);
        assert(h2_esp_platform_safe_call(
            increment, &nested, sizeof(nested), 1024u) == H2_PAL_OK);
        assert(nested == 42 && s_requests == requests && s_locked);
        s_external = (const uint8_t *)&nested; s_external_size = sizeof(nested);
        assert(h2_esp_platform_safe_call_on_worker(
            increment, &nested, sizeof(nested), 1024u) == H2_PAL_ERR_INVALID_STATE);
        assert(h2_esp_platform_safe_call(
            increment, &nested, sizeof(nested), 1024u) == H2_PAL_ERR_INVALID_STATE);
        s_external = NULL;
        /* An Internal first byte cannot hide an external context tail. */
        s_external = (const uint8_t *)&nested + sizeof(nested) - 1u;
        s_external_size = 1u;
        assert(h2_esp_platform_safe_call_on_worker(
            increment, &nested, sizeof(nested), 1024u) == H2_PAL_ERR_INVALID_STATE);
        s_external = NULL;
#if !CONFIG_SPIRAM_XIP_FROM_PSRAM
        s_iram = 0;
        assert(h2_esp_platform_safe_call_on_worker(
            increment, &nested, sizeof(nested), 1024u) == H2_PAL_ERR_INVALID_STATE);
        s_iram = 1;
#endif
        assert(nested == 42 && s_requests == requests && s_locked);
        context->value += nested;
    }
}
static int invoke(callback_context_t *context, int force, int worker) {
    s_expected_caller = context; s_expect_worker = worker;
    return force ? h2_esp_platform_safe_call_on_worker(
        callback, context, sizeof(*context), 4096u) : h2_esp_platform_safe_call(
        callback, context, sizeof(*context), 4096u);
}
static void full_capacity(void *raw) {
    uint8_t *bytes = raw;
    assert(s_worker);
    assert(bytes[0] == 1u && bytes[1023] == 2u);
    bytes[0] = 3u; bytes[1023] = 4u;
}
int main(void) {
    callback_context_t context = {.value = 1};
    /* Existing ordinary Internal/IRAM calls still execute inline, no worker. */
    assert(invoke(&context, 0, 0) == H2_PAL_OK && context.value == 2);
    assert(s_allocations == 0u && s_creations == 0u && s_requests == 0u);
    s_fail_mutex = 1;
    assert(invoke(&context, 1, 1) == H2_PAL_ERR_TASK);
    s_fail_mutex = 0; s_fail_lock = 1;
    assert(invoke(&context, 1, 1) == H2_PAL_ERR_TASK);
    s_fail_lock = 0; s_fail_binary = 1;
    assert(invoke(&context, 1, 1) == H2_PAL_ERR_TASK && !s_locked);
    s_fail_binary = 0; s_fail_alloc = 1;
    assert(invoke(&context, 1, 1) == H2_PAL_ERR_TASK && !s_locked);
    s_fail_alloc = 0; s_fail_task = 1;
    assert(invoke(&context, 1, 1) == H2_PAL_ERR_TASK && !s_locked);
    s_fail_task = 0; s_fail_send = 1;
    assert(invoke(&context, 1, 1) == H2_PAL_ERR_TASK && !s_locked);
    assert(context.value == 2 && s_callbacks == 1u && !s_requests);
    s_fail_send = 0;
    const unsigned allocations = s_allocations, creations = s_creations;
    context.nested = 1;
    assert(invoke(&context, 1, 1) == H2_PAL_OK);
    assert(context.value == 45 && context.bytes[31] == 0x5a && !s_locked);
    assert(s_requests == 1u && s_allocations == allocations && s_creations == creations);
    context.nested = 0; s_internal = 0;
    assert(invoke(&context, 0, 1) == H2_PAL_OK && context.value == 46);
    s_internal = 1;
    assert(invoke(&context, 0, 0) == H2_PAL_OK && context.value == 47);
    assert(invoke(&context, 1, 1) == H2_PAL_OK && context.value == 48);
    assert(s_allocations == allocations && s_creations == creations);
    uint8_t full[1024] = {1}; full[1023] = 2u;
    assert(h2_esp_platform_safe_call_on_worker(
        full_capacity, full, sizeof(full), 16384u) == H2_PAL_OK);
    assert(full[0] == 3u && full[1023] == 4u);
    uint8_t *scratch = NULL; size_t capacity = 0u;
    assert(h2_esp_platform_safe_io_acquire(&scratch, &capacity) == H2_PAL_OK);
    assert(s_scratch_held && scratch != NULL && capacity == 16384u);
    assert(invoke(&context, 1, 1) == H2_PAL_OK && s_scratch_held && !s_locked);
    h2_esp_platform_safe_io_release();
    assert(!s_scratch_held);
    const unsigned callbacks = s_callbacks, requests = s_requests;
    assert(h2_esp_platform_safe_call_on_worker(NULL, &context, sizeof(context), 1024u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_esp_platform_safe_call_on_worker(callback, NULL, sizeof(context), 1024u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_esp_platform_safe_call_on_worker(callback, &context, 0u, 1024u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_esp_platform_safe_call_on_worker(callback, full, 1025u, 1024u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_esp_platform_safe_call_on_worker(callback, &context, sizeof(context), 1023u) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_esp_platform_safe_call_on_worker(callback, &context, sizeof(context), 16385u) == H2_PAL_ERR_INVALID_ARG);
    s_isr = 1;
    assert(invoke(&context, 1, 1) == H2_PAL_ERR_INVALID_ARG);
    assert(invoke(&context, 0, 0) == H2_PAL_ERR_INVALID_ARG);
    assert(s_callbacks == callbacks && s_requests == requests && !s_locked);
    return 0;
}
