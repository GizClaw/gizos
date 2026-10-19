#include "h2_bk_task_tls.h"

#include "FreeRTOS.h"
#include "task.h"
#include <os/mem.h>
#include <os/os.h>
#include <stdlib.h>
#include <string.h>

/* SDK pthread keys (including lwIP's semaphore) own slot 1. The pinned SDK
 * has three slots and no user of slot 2. Do not alter the kernel TCB layout. */
#define H2_BK_TASK_TLS_SLOT 2
#define H2_BK_TASK_TLS_MAGIC UINT32_C(0x4832544c)

#if configNUM_THREAD_LOCAL_STORAGE_POINTERS <= H2_BK_TASK_TLS_SLOT
#error "BK task TLS requires its reserved FreeRTOS slot"
#endif

typedef struct tls_value {
    struct tls_value *next;
    h2_bk_emutls_control_t *control;
    void *data;
    size_t size;
} tls_value_t;

typedef struct task_tls {
    uint32_t magic;
    int error_number;
    tls_value_t *values;
} task_tls_t;

static char initializing;
extern int *__real___errno(void);
extern void *__real___emutls_get_address(h2_bk_emutls_control_t *control);

static TaskHandle_t current_task(void) {
    return rtos_is_in_interrupt_context() ? NULL : xTaskGetCurrentTaskHandle();
}

static task_tls_t *task_context(TaskHandle_t task) {
    void *value = pvTaskGetThreadLocalStoragePointer(task, H2_BK_TASK_TLS_SLOT);
    if (value == &initializing)
        return NULL; /* An allocator's errno access must not recurse. */
    if (value == NULL) {
        vTaskSetThreadLocalStoragePointer(task, H2_BK_TASK_TLS_SLOT, &initializing);
        task_tls_t *context = os_malloc(sizeof(*context));
        if (context == NULL) {
            vTaskSetThreadLocalStoragePointer(task, H2_BK_TASK_TLS_SLOT, NULL);
            abort(); /* TLS cannot safely fall back to another task's storage. */
        }
        memset(context, 0, sizeof(*context));
        context->magic = H2_BK_TASK_TLS_MAGIC;
        vTaskSetThreadLocalStoragePointer(task, H2_BK_TASK_TLS_SLOT, context);
        value = context;
    }
    task_tls_t *context = value;
    if (context->magic != H2_BK_TASK_TLS_MAGIC)
        abort();
    return context;
}

int *__wrap___errno(void) {
    TaskHandle_t task = current_task();
    task_tls_t *context = task == NULL ? NULL : task_context(task);
    return context == NULL ? __real___errno() : &context->error_number;
}

void *__wrap___emutls_get_address(h2_bk_emutls_control_t *control) {
    TaskHandle_t task = current_task();
    if (task == NULL)
        return __real___emutls_get_address(control);
    task_tls_t *context = task_context(task);
    if (context == NULL || control == NULL)
        abort();
    for (tls_value_t *value = context->values; value != NULL; value = value->next)
        if (value->control == control)
            return value->data;

    const size_t alignment = (size_t)control->alignment;
    const size_t size = (size_t)control->size;
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u ||
        alignment - 1u > SIZE_MAX - sizeof(tls_value_t) ||
        size > SIZE_MAX - sizeof(tls_value_t) - (alignment - 1u))
        abort();
    tls_value_t *value = os_malloc(sizeof(*value) + alignment - 1u + size);
    if (value == NULL)
        abort();
    uintptr_t data = (uintptr_t)(value + 1);
    data = (data + alignment - 1u) & ~(uintptr_t)(alignment - 1u);
    value->control = control;
    value->data = (void *)data;
    value->size = size;
    if (control->initial_value != NULL)
        memcpy(value->data, control->initial_value, size);
    else
        memset(value->data, 0, size);
    value->next = context->values;
    context->values = value;
    return value->data;
}

/* Invoked by the kernel's final TCB cleanup, after the task stops running.
 * It therefore handles SDK tasks, PAL tasks and deletion by another core. */
void h2_bk_task_tls_cleanup(void *task) {
    void *stored = pvTaskGetThreadLocalStoragePointer(task, H2_BK_TASK_TLS_SLOT);
    if (stored == NULL || stored == &initializing)
        return;
    task_tls_t *context = stored;
    if (context->magic != H2_BK_TASK_TLS_MAGIC)
        abort();
    vTaskSetThreadLocalStoragePointer(task, H2_BK_TASK_TLS_SLOT, NULL);
    tls_value_t *value = context->values;
    while (value != NULL) {
        tls_value_t *next = value->next;
        volatile unsigned char *data = value->data;
        for (size_t i = 0u; i < value->size; ++i)
            data[i] = 0u;
        os_free(value);
        value = next;
    }
    memset(context, 0, sizeof(*context));
    os_free(context);
}
