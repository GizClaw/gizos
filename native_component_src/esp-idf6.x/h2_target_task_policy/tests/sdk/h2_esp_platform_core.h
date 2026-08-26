#ifndef TEST_H2_ESP_PLATFORM_CORE_H
#define TEST_H2_ESP_PLATFORM_CORE_H

#include "h2/pal/core/h2_pal_errors.h"

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
  h2_esp_task_policy_resolver_t fallback_resolver;
  void *resolver_user;
} h2_esp_task_policy_config_t;

h2_pal_result_t
h2_esp_platform_task_configure(const h2_esp_task_policy_config_t *config);

#endif
