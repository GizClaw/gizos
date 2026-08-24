#include "h2_esp_layout_task_policy.h"

#include "h2_esp_platform_core.h"

#include <stdio.h>
#include <string.h>

typedef struct h2_esp_layout_policy_entry {
  const char *name;
  uint32_t priority;
  h2_esp_task_core_t core;
  uint32_t min_stack_size;
  h2_esp_task_stack_region_t stack_region;
} h2_esp_layout_policy_entry_t;

static const h2_esp_layout_policy_entry_t s_entries[] = {
    {"h2loader/appcmd", 8u, H2_ESP_TASK_CORE_0, 4096u, H2_ESP_TASK_STACK_PSRAM},
    {"h2loader/return", 8u, H2_ESP_TASK_CORE_0, 4096u, H2_ESP_TASK_STACK_PSRAM},
    {"h2loader/blelink", 6u, H2_ESP_TASK_CORE_0, 4096u,
     H2_ESP_TASK_STACK_PSRAM},
    {"bleikcp/kcp", 7u, H2_ESP_TASK_CORE_0, 4096u, H2_ESP_TASK_STACK_PSRAM},
    {"bleikcp/server", 5u, H2_ESP_TASK_CORE_0, 4096u, H2_ESP_TASK_STACK_PSRAM},
    {"h2peer/net", 7u, H2_ESP_TASK_CORE_0, 4096u, H2_ESP_TASK_STACK_PSRAM},
    {"h2peer/udp", 7u, H2_ESP_TASK_CORE_0, 4096u, H2_ESP_TASK_STACK_PSRAM},
};

static h2_pal_result_t resolve_policy(void *user, const char *name,
                                      h2_esp_task_policy_t *out_policy) {
  (void)user;
  if (name == NULL || out_policy == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  for (size_t i = 0u; i < sizeof(s_entries) / sizeof(s_entries[0]); ++i) {
    if (strcmp(name, s_entries[i].name) == 0) {
      *out_policy = (h2_esp_task_policy_t){
          .priority = s_entries[i].priority,
          .core = s_entries[i].core,
          .min_stack_size = s_entries[i].min_stack_size,
          .stack_region = s_entries[i].stack_region,
      };
      return H2_PAL_OK;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}

h2_pal_result_t h2_esp_layout_task_policy_install(void) {
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
  h2_pal_result_t rc = h2_esp_platform_task_configure(&config);
  if (rc == H2_PAL_OK) {
    printf("H2_PAL_TASK_POLICY_READY unit=esp layout=devkit\n");
  } else {
    printf("H2_PAL_TASK_POLICY_FAIL unit=esp stage=configure reason=%d\n", rc);
  }
  return rc;
}
