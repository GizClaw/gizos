#ifndef H2_LUA_RUNTIME_E2E_H
#define H2_LUA_RUNTIME_E2E_H

/** @file h2_lua_runtime_e2e.h @brief Portable Lua Runtime E2E case registry. */

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H2_LUA_RUNTIME_E2E_CASE_COUNT 9u
#define H2_LUA_RUNTIME_E2E_COMPONENT_ID 23u

typedef struct h2_lua_runtime_e2e_case_result {
  const char *id;
  h2_pal_result_t result;
  uint64_t evidence;
} h2_lua_runtime_e2e_case_result_t;

typedef struct h2_lua_runtime_e2e_report {
  const char *scheduler;
  size_t case_count;
  size_t passed;
  h2_lua_runtime_e2e_case_result_t cases[H2_LUA_RUNTIME_E2E_CASE_COUNT];
} h2_lua_runtime_e2e_report_t;

typedef void (*h2_lua_runtime_e2e_case_report_fn)(
    void *user, const h2_lua_runtime_e2e_case_result_t *case_result);

typedef struct h2_lua_runtime_e2e_config {
  const char *scheduler;
  size_t worker_count;
  h2_lua_runtime_e2e_case_report_fn report_case;
  void *report_case_user;
} h2_lua_runtime_e2e_config_t;

/** Synthetic component mapping used only by the App-dispatched event cases. */
const h2_runtime_component_mapper_t *h2_lua_runtime_e2e_component_mapper(void);
const h2_pal_periph_api_t *h2_lua_runtime_e2e_periph_api(void);

h2_pal_result_t
h2_lua_runtime_e2e_run(h2_runtime_t *runtime,
                       const h2_lua_runtime_e2e_config_t *config,
                       h2_lua_runtime_e2e_report_t *out_report);

#ifdef __cplusplus
}
#endif

#endif
