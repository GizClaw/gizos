#include "h2_bk_target_task_policy.h"

#include "h2_bk_platform_core.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef enum h2_bk_target_task_route_mode {
  H2_BK_TARGET_TASK_ROUTE_EXACT = 0,
  H2_BK_TARGET_TASK_ROUTE_PREFIX = 1,
} h2_bk_target_task_route_mode_t;

typedef struct h2_bk_target_task_route {
  const char *name;
  h2_bk_target_task_route_mode_t mode;
  h2_bk_task_policy_t policy;
} h2_bk_target_task_route_t;

#define H2_BK_TARGET_TASK_ROUTE(name_value, mode_value, sdk_name_value,        \
                                core_value, priority_value,                    \
                                min_stack_size_value, stack_region_value)      \
  {                                                                            \
      .name = (name_value),                                                    \
      .mode = (mode_value),                                                    \
      .policy =                                                                \
          {                                                                    \
              .sdk_name = (sdk_name_value),                                    \
              .core = (core_value),                                            \
              .priority = (priority_value),                                    \
              .min_stack_size = (min_stack_size_value),                        \
              .stack_region = (stack_region_value),                            \
          },                                                                   \
  },

static const h2_bk_target_task_route_t s_routes[] = {
    H2_BK_TARGET_TASK_POLICY_ROUTES(H2_BK_TARGET_TASK_ROUTE)};

#undef H2_BK_TARGET_TASK_ROUTE

static h2_pal_result_t resolve_policy(void *user, const char *name,
                                      h2_bk_task_policy_t *out_policy) {
  (void)user;
  if (name == NULL || name[0] == '\0' || out_policy == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  for (size_t i = 0u; i < sizeof(s_routes) / sizeof(s_routes[0]); ++i) {
    if (s_routes[i].mode == H2_BK_TARGET_TASK_ROUTE_EXACT &&
        strcmp(s_routes[i].name, name) == 0) {
      *out_policy = s_routes[i].policy;
      return H2_PAL_OK;
    }
  }
  const h2_bk_target_task_route_t *best = NULL;
  const size_t name_size = strlen(name);
  size_t best_size = 0u;
  for (size_t i = 0u; i < sizeof(s_routes) / sizeof(s_routes[0]); ++i) {
    if (s_routes[i].mode != H2_BK_TARGET_TASK_ROUTE_PREFIX) {
      continue;
    }
    const size_t size = strlen(s_routes[i].name);
    if (size > best_size && name_size > size &&
        strncmp(s_routes[i].name, name, size) == 0) {
      best = &s_routes[i];
      best_size = size;
    }
  }
  if (best == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  *out_policy = best->policy;
  return H2_PAL_OK;
}

static h2_pal_result_t validate_routes(void) {
  for (size_t i = 0u; i < sizeof(s_routes) / sizeof(s_routes[0]); ++i) {
    if (s_routes[i].name == NULL || s_routes[i].name[0] == '\0') {
      return H2_PAL_ERR_INVALID_ARG;
    }
    for (size_t j = i + 1u; j < sizeof(s_routes) / sizeof(s_routes[0]); ++j) {
      if (s_routes[i].mode == s_routes[j].mode &&
          strcmp(s_routes[i].name, s_routes[j].name) == 0) {
        return H2_PAL_ERR_INVALID_ARG;
      }
    }
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_bk_target_task_policy_install(void) {
  h2_pal_result_t rc = validate_routes();
  if (rc != H2_PAL_OK) {
    printf("H2_PAL_TASK_POLICY_FAIL unit=ap target=%s stage=routes reason=%d\n",
           H2_TARGET_TASK_POLICY_NAME, rc);
    return rc;
  }
  const h2_bk_task_policy_config_t config = {
      .resolver = resolve_policy,
      .resolver_user = NULL,
      .unknown_mode = H2_BK_TASK_UNKNOWN_FALLBACK,
      .fallback =
          {
              .sdk_name = NULL,
              .core = 0u,
              .priority = 7u,
              .min_stack_size = 4096u,
              .stack_region = H2_BK_TASK_STACK_DEFAULT,
          },
      .task_allocator = h2_bk_platform_psram_allocator(),
  };
  rc = h2_bk_platform_task_configure(&config);
  if (rc == H2_PAL_OK) {
    printf("H2_PAL_TASK_POLICY_READY unit=ap target=%s\n",
           H2_TARGET_TASK_POLICY_NAME);
  } else {
    printf("H2_PAL_TASK_POLICY_FAIL unit=ap target=%s stage=configure "
           "reason=%d\n",
           H2_TARGET_TASK_POLICY_NAME, rc);
  }
  return rc;
}
