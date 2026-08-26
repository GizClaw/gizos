#include "h2_esp_target_task_policy.h"

#include "h2_esp_platform_core.h"

#include <stdio.h>
#include <string.h>

typedef struct h2_esp_target_task_route {
  const char *name;
  h2_esp_task_policy_t policy;
} h2_esp_target_task_route_t;

#define H2_ESP_TARGET_TASK_ROUTE(name_value, priority_value, core_value,       \
                                 min_stack_size_value, stack_region_value)     \
  {                                                                            \
      .name = (name_value),                                                    \
      .policy =                                                                \
          {                                                                    \
              .priority = (priority_value),                                    \
              .core = (core_value),                                            \
              .min_stack_size = (min_stack_size_value),                        \
              .stack_region = (stack_region_value),                            \
          },                                                                   \
  },

static const h2_esp_target_task_route_t s_routes[] = {
    H2_ESP_TARGET_TASK_POLICY_ROUTES(H2_ESP_TARGET_TASK_ROUTE)};

#undef H2_ESP_TARGET_TASK_ROUTE

static h2_pal_result_t resolve_policy(void *user, const char *name,
                                      h2_esp_task_policy_t *out_policy) {
  (void)user;
  if (name == NULL || name[0] == '\0' || out_policy == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  for (size_t i = 0u; i < sizeof(s_routes) / sizeof(s_routes[0]); ++i) {
    if (strcmp(s_routes[i].name, name) == 0) {
      *out_policy = s_routes[i].policy;
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t validate_routes(void) {
  for (size_t i = 0u; i < sizeof(s_routes) / sizeof(s_routes[0]); ++i) {
    if (s_routes[i].name == NULL || s_routes[i].name[0] == '\0') {
      return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t j = i + 1u; j < sizeof(s_routes) / sizeof(s_routes[0]); ++j) {
      if (strcmp(s_routes[i].name, s_routes[j].name) == 0) {
        return H2_PAL_ERR_INVALID_ARG;
      }
    }
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_esp_target_task_policy_install(void) {
  h2_pal_result_t rc = validate_routes();
  if (rc != H2_PAL_OK) {
    printf("H2_PAL_TASK_POLICY_FAIL unit=esp target=%s stage=routes "
           "reason=%d\n",
           H2_TARGET_TASK_POLICY_NAME, rc);
    return rc;
  }
  static const h2_esp_task_policy_config_t config = {
      .resolver = resolve_policy,
      .resolver_user = NULL,
      .unknown_mode = H2_ESP_TASK_UNKNOWN_FALLBACK,
      .fallback =
          {
              .priority = 4u,
              .core = H2_ESP_TASK_CORE_ANY,
              .min_stack_size = 4096u,
              .stack_region = H2_ESP_TASK_STACK_PSRAM,
          },
  };
  rc = h2_esp_platform_task_configure(&config);
  if (rc == H2_PAL_OK) {
    printf("H2_PAL_TASK_POLICY_READY unit=esp target=%s\n",
           H2_TARGET_TASK_POLICY_NAME);
  } else {
    printf("H2_PAL_TASK_POLICY_FAIL unit=esp target=%s stage=configure "
           "reason=%d\n",
           H2_TARGET_TASK_POLICY_NAME, rc);
  }
  return rc;
}
