#include "h2_trie.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct handler_state {
  const char *name;
  const char *path;
  const char *remainder;
} handler_state_t;

static h2_pal_result_t
record_handler(const void *user, const h2_trie_match_t *match, void *response) {
  handler_state_t *state = (handler_state_t *)response;

  assert(user != NULL);
  assert(match != NULL);
  assert(state != NULL);
  state->name = (const char *)user;
  state->path = match->path;
  state->remainder = match->remainder;
  return H2_PAL_OK;
}

static void test_exact_and_longest_prefix_dispatch(void) {
  static const h2_trie_route_t routes[] = {
      {"api/", H2_TRIE_ROUTE_PREFIX, record_handler, "api"},
      {"api/v1/", H2_TRIE_ROUTE_PREFIX, record_handler, "v1"},
      {"api/v1/status", H2_TRIE_ROUTE_EXACT, record_handler, "status"},
  };
  h2_trie_node_t nodes[64];
  h2_trie_t trie;
  handler_state_t state = {0};

  assert(h2_trie_build(&trie, nodes, 64u, routes,
                       sizeof(routes) / sizeof(routes[0])) == H2_PAL_OK);
  assert(h2_trie_handle(&trie, "api/v1/status", &state) == H2_PAL_OK);
  assert(strcmp(state.name, "status") == 0);
  assert(strcmp(state.remainder, "") == 0);

  memset(&state, 0, sizeof(state));
  assert(h2_trie_handle(&trie, "api/v1/items", &state) == H2_PAL_OK);
  assert(strcmp(state.name, "v1") == 0);
  assert(strcmp(state.path, "api/v1/items") == 0);
  assert(strcmp(state.remainder, "items") == 0);

  memset(&state, 0, sizeof(state));
  assert(h2_trie_handle(&trie, "api/health", &state) == H2_PAL_OK);
  assert(strcmp(state.name, "api") == 0);
  assert(strcmp(state.remainder, "health") == 0);
}

static void test_prefix_requires_a_remainder(void) {
  static const h2_trie_route_t route = {"jobs/", H2_TRIE_ROUTE_PREFIX,
                                        record_handler, "jobs"};
  h2_trie_node_t nodes[16];
  h2_trie_t trie;
  handler_state_t state = {0};

  assert(h2_trie_build(&trie, nodes, 16u, &route, 1u) == H2_PAL_OK);
  assert(h2_trie_handle(&trie, "jobs/", &state) == H2_PAL_ERR_NOT_FOUND);
  assert(h2_trie_handle(&trie, "jobs/run", &state) == H2_PAL_OK);
  assert(strcmp(state.remainder, "run") == 0);
}

static void test_tokenized_lookup(void) {
  static const h2_trie_route_t routes[] = {
      {"  root  ", H2_TRIE_ROUTE_EXACT_OR_PREFIX, record_handler, "root"},
      {"root\tchild", H2_TRIE_ROUTE_EXACT_OR_PREFIX, record_handler, "child"},
  };
  static const char *const exact_tokens[] = {"root", "child"};
  static const char *const prefix_tokens[] = {"root", "child", "argument"};
  static const char *const parent_tokens[] = {"root", "other"};
  h2_trie_node_t nodes[32];
  h2_trie_t trie;
  const h2_trie_route_t *route = NULL;

  assert(h2_trie_build_tokens(&trie, nodes, 32u, routes,
                              sizeof(routes) / sizeof(routes[0])) == H2_PAL_OK);
  assert(h2_trie_find_tokens(&trie, 2u, exact_tokens, &route) == H2_PAL_OK);
  assert(strcmp((const char *)route->user, "child") == 0);
  assert(h2_trie_find_tokens(&trie, 3u, prefix_tokens, &route) == H2_PAL_OK);
  assert(strcmp((const char *)route->user, "child") == 0);
  assert(h2_trie_find_tokens(&trie, 2u, parent_tokens, &route) == H2_PAL_OK);
  assert(strcmp((const char *)route->user, "root") == 0);
}

static void test_validation_and_capacity(void) {
  static const h2_trie_route_t duplicate_routes[] = {
      {"same", H2_TRIE_ROUTE_EXACT, record_handler, "first"},
      {"same", H2_TRIE_ROUTE_EXACT, record_handler, "second"},
  };
  static const h2_trie_route_t route = {"long", H2_TRIE_ROUTE_EXACT,
                                        record_handler, "long"};
  h2_trie_node_t nodes[8];
  h2_trie_t trie;

  assert(h2_trie_build(NULL, nodes, 8u, &route, 1u) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_trie_build(&trie, nodes, 2u, &route, 1u) == H2_PAL_ERR_FULL);
  assert(h2_trie_build(&trie, nodes, 8u, duplicate_routes,
                       sizeof(duplicate_routes) /
                           sizeof(duplicate_routes[0])) == H2_PAL_ERR_FORMAT);
  assert(h2_trie_handle(&trie, "same", NULL) == H2_PAL_ERR_INVALID_ARG);
}

int main(void) {
  test_exact_and_longest_prefix_dispatch();
  test_prefix_requires_a_remainder();
  test_tokenized_lookup();
  test_validation_and_capacity();
  puts("h2_trie tests passed");
  return 0;
}
