#include "h2_trie.h"

#include <string.h>

#define H2_TRIE_NO_INDEX UINT16_MAX

_Static_assert(sizeof(h2_trie_node_t) == 10u,
               "trie nodes must remain compact for embedded callers");

static void h2_trie_node_reset(h2_trie_node_t *node, char symbol) {
  memset(node, 0, sizeof(*node));
  node->first_child = H2_TRIE_NO_INDEX;
  node->next_sibling = H2_TRIE_NO_INDEX;
  node->exact_route = H2_TRIE_NO_INDEX;
  node->prefix_route = H2_TRIE_NO_INDEX;
  node->symbol = symbol;
}

static uint16_t h2_trie_find_child(const h2_trie_t *trie, uint16_t parent,
                                   char symbol) {
  uint16_t child = trie->nodes[parent].first_child;

  while (child != H2_TRIE_NO_INDEX) {
    if (trie->nodes[child].symbol == symbol) {
      return child;
    }
    child = trie->nodes[child].next_sibling;
  }
  return H2_TRIE_NO_INDEX;
}

static int h2_trie_is_ascii_space(char value) {
  return value == ' ' || value == '\t' || value == '\v' || value == '\f';
}

static h2_pal_result_t h2_trie_add_child(h2_trie_t *trie, uint16_t parent,
                                         char symbol, uint16_t *out_child) {
  uint16_t child;

  if (trie->node_count == trie->node_capacity) {
    return H2_PAL_ERR_FULL;
  }
  child = (uint16_t)trie->node_count++;
  h2_trie_node_reset(&trie->nodes[child], symbol);
  trie->nodes[child].next_sibling = trie->nodes[parent].first_child;
  trie->nodes[parent].first_child = child;
  *out_child = child;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_trie_set_route(h2_trie_t *trie, uint16_t node,
                                         h2_trie_route_mode_t mode,
                                         uint16_t route_index) {
  if (mode == H2_TRIE_ROUTE_EXACT || mode == H2_TRIE_ROUTE_EXACT_OR_PREFIX) {
    if (trie->nodes[node].exact_route != H2_TRIE_NO_INDEX) {
      return H2_PAL_ERR_FORMAT;
    }
    trie->nodes[node].exact_route = route_index;
  }
  if (mode == H2_TRIE_ROUTE_PREFIX || mode == H2_TRIE_ROUTE_EXACT_OR_PREFIX) {
    if (trie->nodes[node].prefix_route != H2_TRIE_NO_INDEX) {
      if (mode == H2_TRIE_ROUTE_EXACT_OR_PREFIX) {
        trie->nodes[node].exact_route = H2_TRIE_NO_INDEX;
      }
      return H2_PAL_ERR_FORMAT;
    }
    trie->nodes[node].prefix_route = route_index;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t h2_trie_append_symbol(h2_trie_t *trie, uint16_t *node,
                                             char symbol) {
  uint16_t child = h2_trie_find_child(trie, *node, symbol);

  if (child == H2_TRIE_NO_INDEX) {
    h2_pal_result_t result = h2_trie_add_child(trie, *node, symbol, &child);
    if (result != H2_PAL_OK) {
      return result;
    }
  }
  *node = child;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_trie_add_route(h2_trie_t *trie,
                                         const h2_trie_route_t *route,
                                         uint16_t route_index, int tokenize) {
  uint16_t node = 0u;
  const char *cursor;
  int pending_separator = 0;
  int has_symbol = 0;

  if (route->path == NULL || route->path[0] == '\0' || route->handler == NULL ||
      (route->mode != H2_TRIE_ROUTE_EXACT &&
       route->mode != H2_TRIE_ROUTE_PREFIX &&
       route->mode != H2_TRIE_ROUTE_EXACT_OR_PREFIX)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  for (cursor = route->path; *cursor != '\0'; ++cursor) {
    h2_pal_result_t result;

    if (tokenize && h2_trie_is_ascii_space(*cursor)) {
      pending_separator = has_symbol;
      continue;
    }
    if (pending_separator) {
      result = h2_trie_append_symbol(trie, &node, ' ');
      if (result != H2_PAL_OK) {
        return result;
      }
      pending_separator = 0;
    }
    result = h2_trie_append_symbol(trie, &node, *cursor);
    if (result != H2_PAL_OK) {
      return result;
    }
    has_symbol = 1;
  }
  if (!has_symbol) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return h2_trie_set_route(trie, node, route->mode, route_index);
}

static h2_pal_result_t
h2_trie_build_internal(h2_trie_t *trie, h2_trie_node_t *nodes,
                       size_t node_capacity, const h2_trie_route_t *routes,
                       size_t route_count, int tokenize) {
  size_t index;

  if (trie == NULL || nodes == NULL || node_capacity == 0u ||
      node_capacity >= H2_TRIE_NO_INDEX || route_count >= H2_TRIE_NO_INDEX ||
      (route_count != 0u && routes == NULL)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(trie, 0, sizeof(*trie));
  trie->nodes = nodes;
  trie->routes = routes;
  trie->node_capacity = node_capacity;
  trie->route_count = route_count;
  trie->node_count = 1u;
  h2_trie_node_reset(&trie->nodes[0], '\0');
  for (index = 0u; index < route_count; ++index) {
    h2_pal_result_t result =
        h2_trie_add_route(trie, &routes[index], (uint16_t)index, tokenize);
    if (result != H2_PAL_OK) {
      trie->node_count = 0u;
      return result;
    }
  }
  trie->initialized = 1;
  return H2_PAL_OK;
}

h2_pal_result_t h2_trie_build(h2_trie_t *trie, h2_trie_node_t *nodes,
                              size_t node_capacity,
                              const h2_trie_route_t *routes,
                              size_t route_count) {
  return h2_trie_build_internal(trie, nodes, node_capacity, routes, route_count,
                                0);
}

h2_pal_result_t h2_trie_build_tokens(h2_trie_t *trie, h2_trie_node_t *nodes,
                                     size_t node_capacity,
                                     const h2_trie_route_t *routes,
                                     size_t route_count) {
  return h2_trie_build_internal(trie, nodes, node_capacity, routes, route_count,
                                1);
}

h2_pal_result_t h2_trie_handle(const h2_trie_t *trie, const char *path,
                               void *response) {
  const h2_trie_route_t *prefix_route = NULL;
  const char *prefix_remainder = NULL;
  const char *cursor;
  uint16_t node = 0u;

  if (trie == NULL || path == NULL || path[0] == '\0' || !trie->initialized ||
      trie->nodes == NULL || trie->node_count == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  for (cursor = path; *cursor != '\0'; ++cursor) {
    uint16_t child;

    if (trie->nodes[node].prefix_route != H2_TRIE_NO_INDEX) {
      prefix_route = &trie->routes[trie->nodes[node].prefix_route];
      prefix_remainder = cursor;
    }
    child = h2_trie_find_child(trie, node, *cursor);
    if (child == H2_TRIE_NO_INDEX) {
      break;
    }
    node = child;
  }
  if (*cursor == '\0' && trie->nodes[node].exact_route != H2_TRIE_NO_INDEX) {
    const h2_trie_route_t *route = &trie->routes[trie->nodes[node].exact_route];
    const h2_trie_match_t match = {
        .path = path,
        .remainder = cursor,
    };
    return route->handler(route->user, &match, response);
  }
  if (prefix_route != NULL) {
    const h2_trie_match_t match = {
        .path = path,
        .remainder = prefix_remainder,
    };
    return prefix_route->handler(prefix_route->user, &match, response);
  }
  return H2_PAL_ERR_NOT_FOUND;
}

h2_pal_result_t h2_trie_find_tokens(const h2_trie_t *trie, size_t token_count,
                                    const char *const *tokens,
                                    const h2_trie_route_t **out_route) {
  const h2_trie_route_t *prefix_route = NULL;
  uint16_t node = 0u;
  size_t token_index;
  int matched = 1;

  if (trie == NULL || token_count == 0u || tokens == NULL ||
      out_route == NULL || !trie->initialized || trie->nodes == NULL ||
      trie->node_count == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_route = NULL;
  for (token_index = 0u; token_index < token_count; ++token_index) {
    const char *cursor = tokens[token_index];

    if (cursor == NULL || cursor[0] == '\0') {
      return H2_PAL_ERR_INVALID_ARG;
    }
    if (token_index != 0u) {
      uint16_t child;

      if (trie->nodes[node].prefix_route != H2_TRIE_NO_INDEX) {
        prefix_route = &trie->routes[trie->nodes[node].prefix_route];
      }
      child = h2_trie_find_child(trie, node, ' ');
      if (child == H2_TRIE_NO_INDEX) {
        matched = 0;
        break;
      }
      node = child;
    }
    for (; *cursor != '\0'; ++cursor) {
      if (h2_trie_is_ascii_space(*cursor)) {
        return H2_PAL_ERR_INVALID_ARG;
      }
      uint16_t child = h2_trie_find_child(trie, node, *cursor);

      if (child == H2_TRIE_NO_INDEX) {
        matched = 0;
        break;
      }
      node = child;
    }
    if (!matched) {
      break;
    }
  }
  if (matched && token_index == token_count &&
      trie->nodes[node].exact_route != H2_TRIE_NO_INDEX) {
    *out_route = &trie->routes[trie->nodes[node].exact_route];
    return H2_PAL_OK;
  }
  if (prefix_route != NULL) {
    *out_route = prefix_route;
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_NOT_FOUND;
}
