#include "h2_esp_target_task_policy.h"

#include "h2_esp_platform_core.h"
#include "h2_trie.h"

#include <stdio.h>

#define POLICY(priority_value)                                                 \
  {                                                                            \
      .priority = (priority_value),                                            \
      .core = H2_ESP_TASK_CORE_0,                                              \
      .min_stack_size = 4096u,                                                 \
      .stack_region = H2_ESP_TASK_STACK_PSRAM,                                 \
  }

static const h2_esp_task_policy_t s_priority_8_policy = POLICY(8u);
static const h2_esp_task_policy_t s_priority_7_policy = POLICY(7u);
static const h2_esp_task_policy_t s_priority_6_policy = POLICY(6u);
static const h2_esp_task_policy_t s_priority_5_policy = POLICY(5u);

static h2_pal_result_t
handle_policy(const void *user, const h2_trie_match_t *match, void *response) {
  (void)match;
  if (user == NULL || response == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *(h2_esp_task_policy_t *)response = *(const h2_esp_task_policy_t *)user;
  return H2_PAL_OK;
}

static const h2_trie_route_t s_routes[] = {
    {"h2loader/appcmd", H2_TRIE_ROUTE_EXACT, handle_policy,
     &s_priority_8_policy},
    {"h2loader/return", H2_TRIE_ROUTE_EXACT, handle_policy,
     &s_priority_8_policy},
    {"h2loader/blelink", H2_TRIE_ROUTE_EXACT, handle_policy,
     &s_priority_6_policy},
    {"bleikcp/kcp", H2_TRIE_ROUTE_EXACT, handle_policy, &s_priority_7_policy},
    {"bleikcp/server", H2_TRIE_ROUTE_EXACT, handle_policy,
     &s_priority_5_policy},
    {"h2peer/net", H2_TRIE_ROUTE_EXACT, handle_policy, &s_priority_7_policy},
    {"h2peer/udp", H2_TRIE_ROUTE_EXACT, handle_policy, &s_priority_7_policy},
};

enum {
  ROUTE_NODE_CAPACITY = 1u + H2_TRIE_LITERAL_NODE_COUNT("h2loader/appcmd") +
                        H2_TRIE_LITERAL_NODE_COUNT("h2loader/return") +
                        H2_TRIE_LITERAL_NODE_COUNT("h2loader/blelink") +
                        H2_TRIE_LITERAL_NODE_COUNT("bleikcp/kcp") +
                        H2_TRIE_LITERAL_NODE_COUNT("bleikcp/server") +
                        H2_TRIE_LITERAL_NODE_COUNT("h2peer/net") +
                        H2_TRIE_LITERAL_NODE_COUNT("h2peer/udp"),
};

static h2_trie_node_t s_route_nodes[ROUTE_NODE_CAPACITY];
static h2_trie_t s_router;

static h2_pal_result_t resolve_policy(void *user, const char *name,
                                      h2_esp_task_policy_t *out_policy) {
  (void)user;
  if (name == NULL || name[0] == '\0' || out_policy == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  return h2_trie_handle(&s_router, name, out_policy);
}

static h2_pal_result_t
resolve_fallback_policy(void *user, const char *name,
                        h2_esp_task_policy_t *out_policy) {
  (void)user;
  (void)name;
  if (out_policy == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_policy = (h2_esp_task_policy_t){
      .priority = 4u,
      .core = H2_ESP_TASK_CORE_ANY,
      .min_stack_size = 4096u,
      .stack_region = H2_ESP_TASK_STACK_PSRAM,
  };
  return H2_PAL_OK;
}

h2_pal_result_t h2_esp_target_task_policy_install(void) {
  h2_pal_result_t rc =
      h2_trie_build(&s_router, s_route_nodes, ROUTE_NODE_CAPACITY, s_routes,
                    sizeof(s_routes) / sizeof(s_routes[0]));
  if (rc != H2_PAL_OK) {
    printf("H2_PAL_TASK_POLICY_FAIL unit=esp "
           "target=projects/h2loader/targets/h2loader_tar_zlib/loader/"
           "waveshare_esp32s3_a7670e_4g "
           "stage=routes reason=%d\n",
           rc);
    return rc;
  }
  static const h2_esp_task_policy_config_t config = {
      .resolver = resolve_policy,
      .fallback_resolver = resolve_fallback_policy,
      .resolver_user = NULL,
  };
  rc = h2_esp_platform_task_configure(&config);
  if (rc == H2_PAL_OK) {
    printf("H2_PAL_TASK_POLICY_READY unit=esp "
           "target=projects/h2loader/targets/h2loader_tar_zlib/loader/"
           "waveshare_esp32s3_a7670e_4g\n");
  } else {
    printf("H2_PAL_TASK_POLICY_FAIL unit=esp "
           "target=projects/h2loader/targets/h2loader_tar_zlib/loader/"
           "waveshare_esp32s3_a7670e_4g "
           "stage=configure reason=%d\n",
           rc);
  }
  return rc;
}
