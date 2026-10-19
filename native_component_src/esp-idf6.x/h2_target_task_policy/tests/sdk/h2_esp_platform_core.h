#ifndef TEST_H2_ESP_PLATFORM_CORE_H
#define TEST_H2_ESP_PLATFORM_CORE_H

#include "h2/pal/core/h2_pal_errors.h"
#include "h2/pal/os/h2_pal_mem.h"

#include <stdint.h>

typedef enum {
  H2_ESP_TASK_CORE_ANY = -1,
  H2_ESP_TASK_CORE_0 = 0,
  H2_ESP_TASK_CORE_1 = 1,
} h2_esp_task_core_t;

typedef enum {
  H2_ESP_TASK_STACK_INTERNAL = 0,
  H2_ESP_TASK_STACK_PSRAM = 1,
} h2_esp_task_stack_region_t;

typedef struct h2_esp_task_policy {
  uint32_t priority;
  h2_esp_task_core_t core;
  uint32_t min_stack_size;
  h2_esp_task_stack_region_t stack_region;
} h2_esp_task_policy_t;

typedef h2_pal_result_t (*h2_esp_task_policy_resolver_t)(
    void *, const char *, h2_esp_task_policy_t *);

typedef struct h2_esp_task_policy_config {
  h2_esp_task_policy_resolver_t resolver;
  void *resolver_user;
  /** Borrowed PSRAM stack allocator with alloc/free; NULL keeps WithCaps.
   * Used only when the resolved policy selects PSRAM. Must provide 8-bit
   * PSRAM storage aligned for StackType_t and live until all tasks join.
   * TCB storage remains internal RAM; internal-policy stacks ignore this. */
  const h2_pal_mem_api_t *psram_stack_allocator;
} h2_esp_task_policy_config_t;

h2_pal_result_t
h2_esp_platform_task_configure(const h2_esp_task_policy_config_t *config);

#endif
