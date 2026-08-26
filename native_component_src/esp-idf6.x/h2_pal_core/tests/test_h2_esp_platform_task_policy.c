#include "h2_esp_platform_core.h"

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

void h2_esp_platform_task_test_reset(void);

typedef struct test_state {
  int fail_semaphore;
  int fail_create;
  int fail_join;
  int creates;
  int deletes;
  uint32_t stack_size;
  uint32_t caps;
  UBaseType_t priority;
  BaseType_t core;
  const char *name;
  const char *fallback_name;
} test_state_t;

static test_state_t s;
static int s_semaphore;

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
  return s.fail_semaphore ? NULL : &s_semaphore;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
  assert(semaphore == &s_semaphore);
  return pdTRUE;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout) {
  assert(semaphore == &s_semaphore && timeout == portMAX_DELAY);
  return s.fail_join ? 0 : pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t semaphore) {
  assert(semaphore == &s_semaphore);
  s.deletes++;
}
static BaseType_t capture_create(const char *name, uint32_t stack_size,
                                 UBaseType_t priority, TaskHandle_t *out_task,
                                 BaseType_t core, uint32_t caps) {
  s.creates++;
  s.name = name;
  s.stack_size = stack_size;
  s.priority = priority;
  s.core = core;
  s.caps = caps;
  *out_task = (TaskHandle_t)(uintptr_t)1u;
  return s.fail_create ? 0 : pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry, const char *name,
                                   uint32_t stack_size, void *ctx,
                                   UBaseType_t priority, TaskHandle_t *out_task,
                                   BaseType_t core) {
  assert(entry != NULL && ctx != NULL);
  return capture_create(name, stack_size, priority, out_task, core, 0u);
}
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t entry,
                                           const char *name,
                                           uint32_t stack_size, void *ctx,
                                           UBaseType_t priority,
                                           TaskHandle_t *out_task,
                                           BaseType_t core, uint32_t caps) {
  assert(entry != NULL && ctx != NULL);
  return capture_create(name, stack_size, priority, out_task, core, caps);
}
void vTaskDelete(TaskHandle_t task) { assert(task == NULL); }
void vTaskDeleteWithCaps(TaskHandle_t task) { assert(task == NULL); }

static h2_pal_result_t resolve(void *user, const char *name,
                               h2_esp_task_policy_t *out) {
  (void)user;
  if (name != NULL && strcmp(name, "known") == 0) {
    *out = (h2_esp_task_policy_t){
        .priority = 9u,
        .core = H2_ESP_TASK_CORE_1,
        .min_stack_size = 8192u,
        .stack_region = H2_ESP_TASK_STACK_INTERNAL,
    };
    return H2_PAL_OK;
  }
  if (name != NULL && strcmp(name, "invalid") == 0) {
    *out = (h2_esp_task_policy_t){
        .priority = configMAX_PRIORITIES,
        .core = H2_ESP_TASK_CORE_0,
        .min_stack_size = 4096u,
        .stack_region = H2_ESP_TASK_STACK_INTERNAL,
    };
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t resolve_fallback(void *user, const char *name,
                                        h2_esp_task_policy_t *out) {
  assert(user == &s);
  s.fallback_name = name;
  if (name != NULL && strcmp(name, "fallback-reject") == 0) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (name != NULL && strcmp(name, "fallback-error") == 0) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  if (name != NULL && strcmp(name, "fallback-invalid") == 0) {
    *out = (h2_esp_task_policy_t){
        .priority = configMAX_PRIORITIES,
        .core = H2_ESP_TASK_CORE_0,
        .min_stack_size = 4096u,
        .stack_region = H2_ESP_TASK_STACK_INTERNAL,
    };
    return H2_PAL_OK;
  }
  *out = (h2_esp_task_policy_t){
      .priority = name != NULL && strcmp(name, "dynamic-high") == 0 ? 6u : 3u,
      .core = H2_ESP_TASK_CORE_ANY,
      .min_stack_size =
          name != NULL && strcmp(name, "dynamic-high") == 0 ? 12288u : 6144u,
      .stack_region = H2_ESP_TASK_STACK_PSRAM,
  };
  return H2_PAL_OK;
}

static h2_esp_task_policy_config_t
config(h2_esp_task_policy_resolver_t fallback_resolver) {
  return (h2_esp_task_policy_config_t){
      .resolver = resolve,
      .fallback_resolver = fallback_resolver,
      .resolver_user = &s,
  };
}

static void entry(void *user) { (void)user; }
static void reset(void) {
  h2_esp_platform_task_test_reset();
  s = (test_state_t){0};
}

int main(void) {
  const h2_pal_task_api_t *api = h2_esp_platform_task_api();
  h2_pal_task_t *task = (h2_pal_task_t *)(uintptr_t)7u;
  h2_pal_task_options_t options = {.name = "known", .min_stack_size = 1024u};

  reset();
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(task == NULL);
  h2_esp_task_policy_config_t cfg = config(resolve_fallback);
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_ERR_INVALID_STATE);

  reset();
  assert(h2_esp_platform_task_configure(NULL) == H2_PAL_ERR_INVALID_ARG);
  cfg = config(resolve_fallback);
  cfg.resolver = NULL;
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_ERR_INVALID_ARG);
  cfg = config(resolve_fallback);
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_ERR_INVALID_STATE);
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) == H2_PAL_OK);
  assert(task != NULL && s.creates == 1 && s.priority == 9u && s.core == 1 &&
         s.stack_size == 8192u && s.caps == 0u);
  s.fail_join = 1;
  assert(api->vtable->join(NULL, task) == H2_PAL_ERR_TASK);
  s.fail_join = 0;
  assert(api->vtable->join(NULL, task) == H2_PAL_OK);

  reset();
  cfg = config(NULL);
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  options.name = "unknown";
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_NOT_FOUND);
  assert(s.creates == 0 && task == NULL);

  reset();
  cfg = config(resolve_fallback);
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  options.name = "dynamic-high";
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) == H2_PAL_OK);
  assert(strcmp(s.fallback_name, "dynamic-high") == 0 && s.priority == 6u &&
         s.stack_size == 12288u && s.core == tskNO_AFFINITY &&
         s.caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  assert(api->vtable->join(NULL, task) == H2_PAL_OK);

  reset();
  cfg = config(resolve_fallback);
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  options.name = "fallback-invalid";
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_TASK);
  assert(s.creates == 0);

  reset();
  cfg = config(resolve_fallback);
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  options.name = "invalid";
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_TASK);
  assert(s.creates == 0);

  reset();
  cfg = config(resolve_fallback);
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  options.name = "fallback-reject";
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_NOT_FOUND);
  options.name = "fallback-error";
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_TASK);
  assert(s.creates == 0);

  reset();
  cfg = config(resolve_fallback);
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  s.fail_semaphore = 1;
  options.name = "known";
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_NO_MEMORY);
  s.fail_semaphore = 0;
  s.fail_create = 1;
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_TASK);
  assert(s.deletes == 1);

#if SIZE_MAX > UINT32_MAX
  reset();
  cfg = config(resolve_fallback);
  assert(h2_esp_platform_task_configure(&cfg) == H2_PAL_OK);
  options.min_stack_size = (size_t)UINT32_MAX + 1u;
  assert(api->vtable->start(NULL, &options, entry, NULL, &task) ==
         H2_PAL_ERR_TASK);
  assert(s.creates == 0 && task == NULL);
#endif
  return 0;
}
