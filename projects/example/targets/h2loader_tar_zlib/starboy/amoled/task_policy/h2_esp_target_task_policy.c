#include "h2_esp_target_task_policy.h"

#include "h2_bleikcp_task_names.h"

#include "h2_peer_task_names.h"

#include "h2loader_app_task_names.h"
#include "h2_loader_task_names.h"

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
    {H2LOADER_APP_COMMAND_TASK_NAME_VALUE, H2_TRIE_ROUTE_EXACT, handle_policy,
     &s_priority_8_policy},
    {H2_LOADER_RETURN_TASK_NAME_VALUE, H2_TRIE_ROUTE_EXACT, handle_policy,
     &s_priority_8_policy},
    {H2_LOADER_BLE_LINK_TASK_NAME_VALUE, H2_TRIE_ROUTE_EXACT, handle_policy,
     &s_priority_6_policy},
    {H2_BLEIKCP_WORKER_TASK_NAME_VALUE, H2_TRIE_ROUTE_EXACT, handle_policy, &s_priority_7_policy},
    {H2_BLEIKCP_SERVER_TASK_NAME_VALUE, H2_TRIE_ROUTE_EXACT, handle_policy,
     &s_priority_5_policy},
    {H2_PEER_NETWORK_TASK_NAME_VALUE, H2_TRIE_ROUTE_EXACT, handle_policy, &s_priority_7_policy},
    {H2_PEER_UDP_TASK_NAME_VALUE, H2_TRIE_ROUTE_EXACT, handle_policy, &s_priority_7_policy},
};

enum {
  ROUTE_NODE_CAPACITY = 1u + H2_TRIE_LITERAL_NODE_COUNT(H2LOADER_APP_COMMAND_TASK_NAME_VALUE) +
                        H2_TRIE_LITERAL_NODE_COUNT(H2_LOADER_RETURN_TASK_NAME_VALUE) +
                        H2_TRIE_LITERAL_NODE_COUNT(H2_LOADER_BLE_LINK_TASK_NAME_VALUE) +
                        H2_TRIE_LITERAL_NODE_COUNT(H2_BLEIKCP_WORKER_TASK_NAME_VALUE) +
                        H2_TRIE_LITERAL_NODE_COUNT(H2_BLEIKCP_SERVER_TASK_NAME_VALUE) +
                        H2_TRIE_LITERAL_NODE_COUNT(H2_PEER_NETWORK_TASK_NAME_VALUE) +
                        H2_TRIE_LITERAL_NODE_COUNT(H2_PEER_UDP_TASK_NAME_VALUE),
};

static h2_trie_node_t s_route_nodes[ROUTE_NODE_CAPACITY];
static h2_trie_t s_router;

static h2_pal_result_t resolve_policy(void *user, const char *name,
                                      h2_esp_task_policy_t *out_policy) {
  (void)user;
  /* Reject invalid inputs before route or fallback dispatch. */
  if (name == NULL || name[0] == '\0' || out_policy == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  return h2_trie_handle(&s_router, name, out_policy);
}

static h2_pal_result_t handle_default_policy(const void *user,
                                             const h2_trie_match_t *match,
                                             void *response) {
  (void)user;
  (void)match;
  if (response == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *(h2_esp_task_policy_t *)response = (h2_esp_task_policy_t){
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
           "target=projects/example/targets/h2loader_tar_zlib/starboy/amoled "
           "stage=routes reason=%d\n",
           rc);
    return rc;
  }
  rc = h2_trie_set_fallback(&s_router, handle_default_policy, NULL);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  static const h2_esp_task_policy_config_t config = {
      .resolver = resolve_policy,
      .resolver_user = NULL,
  };
  rc = h2_esp_platform_task_configure(&config);
  if (rc == H2_PAL_OK) {
    printf(
        "H2_PAL_TASK_POLICY_READY unit=esp "
        "target=projects/example/targets/h2loader_tar_zlib/starboy/amoled\n");
  } else {
    printf("H2_PAL_TASK_POLICY_FAIL unit=esp "
           "target=projects/example/targets/h2loader_tar_zlib/starboy/amoled "
           "stage=configure reason=%d\n",
           rc);
  }
  return rc;
}
