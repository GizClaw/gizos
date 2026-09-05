#include "h2_esp_platform_safe_call.h"

#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct test_task_capture {
  const char *name;
  uint32_t stack_size;
  UBaseType_t priority;
  BaseType_t core;
  TaskFunction_t entry;
  void *context;
} test_task_capture_t;

static test_task_capture_t s_task;
static unsigned char s_stack[16384];
static int s_task_handle;

void *heap_caps_malloc(size_t size, unsigned int caps) {
  assert(size == sizeof(s_stack));
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return s_stack;
}

int esp_ptr_internal(const void *pointer) {
  (void)pointer;
  return 0;
}

int esp_ptr_in_iram(const void *pointer) {
  (void)pointer;
  return 0;
}

BaseType_t xPortInIsrContext(void) { return 0; }

SemaphoreHandle_t xSemaphoreCreateBinary(void) { return NULL; }

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage) {
  assert(storage != NULL);
  return storage;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) {
  assert(storage != NULL);
  return storage;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
  assert(semaphore != NULL);
  return pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout) {
  assert(semaphore != NULL);
  assert(timeout == portMAX_DELAY);
  return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore) { (void)semaphore; }

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char *name,
                                   uint32_t stack_size, void *ctx,
                                   UBaseType_t priority,
                                   TaskHandle_t *out_task, BaseType_t core) {
  (void)entry;
  (void)name;
  (void)stack_size;
  (void)ctx;
  (void)priority;
  (void)out_task;
  (void)core;
  return 0;
}

TaskHandle_t xTaskCreateStaticPinnedToCore(
    TaskFunction_t entry, const char *name, uint32_t stack_size, void *ctx,
    UBaseType_t priority, StackType_t *stack, StaticTask_t *task_storage,
    BaseType_t core) {
  assert(entry != NULL);
  assert(stack == s_stack);
  assert(task_storage != NULL);
  s_task = (test_task_capture_t){
      .name = name,
      .stack_size = stack_size,
      .priority = priority,
      .core = core,
      .entry = entry,
      .context = ctx,
  };
  return &s_task_handle;
}

StackType_t *xTaskGetStackStart(TaskHandle_t task) {
  assert(task == NULL);
  return NULL;
}

void vTaskDelete(TaskHandle_t task) { (void)task; }

static void callback(void *context) {
  int *value = (int *)context;
  *value = 2;
}

int main(void) {
  int value = 1;
  assert(h2_esp_platform_safe_call(callback, &value, sizeof(value), 1024u) ==
         H2_PAL_OK);
  assert(s_task.name != NULL);
  assert(strcmp(s_task.name, "$esp/safe-call") == 0);
  assert(s_task.stack_size == 16384u);
  assert(s_task.priority == 9u);
  assert(s_task.core == 0);
  assert(s_task.entry != NULL);
  assert(s_task.context != NULL);
  return 0;
}
