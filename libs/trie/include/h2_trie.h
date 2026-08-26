#ifndef H2_TRIE_H
#define H2_TRIE_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum new character nodes needed when registering one string literal. */
#define H2_TRIE_LITERAL_NODE_COUNT(path_literal) (sizeof(path_literal) - 1u)

typedef struct h2_trie h2_trie_t;

/** @brief Route match passed to a synchronous trie handler. */
typedef struct h2_trie_match {
  const char *path;
  const char *remainder;
} h2_trie_match_t;

/** @brief Synchronous route handler. All pointers are borrowed for the call. */
typedef h2_pal_result_t (*h2_trie_handler_fn)(const void *user,
                                              const h2_trie_match_t *match,
                                              void *response);

typedef enum h2_trie_route_mode {
  H2_TRIE_ROUTE_EXACT = 0,
  H2_TRIE_ROUTE_PREFIX = 1,
  H2_TRIE_ROUTE_EXACT_OR_PREFIX = 2,
} h2_trie_route_mode_t;

/** @brief Immutable route definition consumed by h2_trie_build(). */
typedef struct h2_trie_route {
  const char *path;
  h2_trie_route_mode_t mode;
  h2_trie_handler_fn handler;
  const void *user;
} h2_trie_route_t;

/** @brief One caller-owned trie node. Treat fields as private. */
typedef struct h2_trie_node {
  uint16_t first_child;
  uint16_t next_sibling;
  uint16_t exact_route;
  uint16_t prefix_route;
  char symbol;
} h2_trie_node_t;

/** @brief Caller-owned trie state. Treat fields as private. */
struct h2_trie {
  h2_trie_node_t *nodes;
  const h2_trie_route_t *routes;
  h2_trie_handler_fn fallback_handler;
  const void *fallback_user;
  size_t node_count;
  size_t node_capacity;
  size_t route_count;
  int initialized;
};

/**
 * @brief Build a route trie in caller-owned storage without heap allocation.
 *
 * Exact routes match the complete path. Prefix routes require at least one
 * character after the registered prefix. Duplicate route/mode pairs are
 * rejected. Route strings and handler users remain borrowed by the trie.
 */
h2_pal_result_t h2_trie_build(h2_trie_t *trie, h2_trie_node_t *nodes,
                              size_t node_capacity,
                              const h2_trie_route_t *routes,
                              size_t route_count);

/**
 * @brief Build a trie from whitespace-delimited token paths.
 *
 * Leading, trailing, and repeated ASCII whitespace in route paths is
 * normalized to one logical separator. This is useful for command routers
 * that dispatch an already-tokenized argv without joining it into a buffer.
 */
h2_pal_result_t h2_trie_build_tokens(h2_trie_t *trie, h2_trie_node_t *nodes,
                                     size_t node_capacity,
                                     const h2_trie_route_t *routes,
                                     size_t route_count);

/**
 * @brief Register a synchronous handler for paths that match no route.
 *
 * The fallback uses the same handler contract as a route. Its match path is
 * the complete input path and its remainder is also the complete input path.
 * Exact and prefix routes always take precedence.
 */
h2_pal_result_t h2_trie_set_fallback(h2_trie_t *trie,
                                     h2_trie_handler_fn handler,
                                     const void *user);

/**
 * @brief Dispatch a path to the best route.
 *
 * A complete exact route wins over prefix routes. Otherwise the deepest
 * matching prefix route is selected. If no route matches, the registered
 * fallback handler is selected. The handler executes synchronously.
 */
h2_pal_result_t h2_trie_handle(const h2_trie_t *trie, const char *path,
                               void *response);

/**
 * @brief Find the best route for a whitespace-tokenized path.
 *
 * Exact full-path routes win. Otherwise the deepest prefix ending at a token
 * boundary is returned. No joined path or temporary allocation is required.
 */
h2_pal_result_t h2_trie_find_tokens(const h2_trie_t *trie, size_t token_count,
                                    const char *const *tokens,
                                    const h2_trie_route_t **out_route);

#ifdef __cplusplus
}
#endif

#endif
