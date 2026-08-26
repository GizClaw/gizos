#include "h2_bk_target_task_policy.h"

#include "h2_bk_platform_core.h"
#include "h2_trie.h"

#include <stdio.h>

static const h2_bk_task_policy_t s_h2loader_policy = {
    .sdk_name = NULL,
    .core = 0u,
    .priority = 5u,
    .min_stack_size = 8192u,
    .stack_region = H2_BK_TASK_STACK_PSRAM,
};

static h2_pal_result_t
handle_policy(const void *user, const h2_trie_match_t *match, void *response) {
  (void)match;
  if (user == NULL || response == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *(h2_bk_task_policy_t *)response = *(const h2_bk_task_policy_t *)user;
  return H2_PAL_OK;
}

static const h2_trie_route_t s_routes[] = {
    {"h2loader/appcmd", H2_TRIE_ROUTE_EXACT, handle_policy, &s_h2loader_policy},
    {"h2loader/uartcmd", H2_TRIE_ROUTE_EXACT, handle_policy,
     &s_h2loader_policy},
};

enum {
  ROUTE_NODE_CAPACITY = 1u + H2_TRIE_LITERAL_NODE_COUNT("h2loader/appcmd") +
                        H2_TRIE_LITERAL_NODE_COUNT("h2loader/uartcmd"),
};

static h2_trie_node_t s_route_nodes[ROUTE_NODE_CAPACITY];
static h2_trie_t s_router;

static h2_pal_result_t resolve_policy(void *user, const char *name,
                                      h2_bk_task_policy_t *out_policy) {
  (void)user;
  if (name == NULL || name[0] == '\0' || out_policy == NULL) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  return h2_trie_handle(&s_router, name, out_policy);
}

h2_pal_result_t h2_bk_target_task_policy_install(void) {
  h2_pal_result_t rc =
      h2_trie_build(&s_router, s_route_nodes, ROUTE_NODE_CAPACITY, s_routes,
                    sizeof(s_routes) / sizeof(s_routes[0]));
  if (rc != H2_PAL_OK) {
    printf("H2_PAL_TASK_POLICY_FAIL unit=ap "
           "target=projects/example/targets/h2loader_tar_zlib/ble-observer/"
           "bk7258_v3_202405 "
           "stage=routes reason=%d\n",
           rc);
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
    printf("H2_PAL_TASK_POLICY_READY unit=ap "
           "target=projects/example/targets/h2loader_tar_zlib/ble-observer/"
           "bk7258_v3_202405\n");
  } else {
    printf("H2_PAL_TASK_POLICY_FAIL unit=ap "
           "target=projects/example/targets/h2loader_tar_zlib/ble-observer/"
           "bk7258_v3_202405 "
           "stage=configure reason=%d\n",
           rc);
  }
  return rc;
}
