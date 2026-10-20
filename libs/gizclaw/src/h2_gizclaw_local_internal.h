#ifndef H2_GIZCLAW_LOCAL_INTERNAL_H
#define H2_GIZCLAW_LOCAL_INTERNAL_H
#include "h2_gizclaw_local.h"
typedef struct h2_gizclaw_local h2_gizclaw_local_t;
typedef struct h2_gizclaw_local_config {
  const h2_pal_task_api_t* task;
  const h2_pal_sync_api_t* sync;
  size_t request_capacity, request_limit_bytes;
} h2_gizclaw_local_config_t;
h2_pal_result_t h2_gizclaw_local_create_internal(
    const h2_gizclaw_local_config_t*, const h2_gizclaw_config_t*,
    h2_gizclaw_local_t**);
h2_pal_result_t h2_gizclaw_local_start_internal(h2_gizclaw_local_t*);
h2_pal_result_t h2_gizclaw_local_stop_internal(h2_gizclaw_local_t*);
h2_pal_result_t h2_gizclaw_local_destroy_internal(h2_gizclaw_local_t**);
bool h2_gizclaw_local_quiescent_internal(h2_gizclaw_local_t*);
void h2_gizclaw_local_cancel_internal(h2_gizclaw_local_t*);
h2_gizclaw_local_t* h2_gizclaw_local_internal(h2_gizclaw_service_t*);
int h2_gizclaw_local_network_tool_internal(void*, h2_gizclaw_tool_t,
                                           h2_gizclaw_rpc_bytes_t,
                                           h2_gizclaw_rpc_provider_response_t*);
int h2_gizclaw_local_network_mhs_internal(h2_gizclaw_service_t*,
                                          h2_gizclaw_rpc_method_t,
                                          h2_gizclaw_rpc_bytes_t,
                                          h2_gizclaw_rpc_provider_response_t*);
const h2_gizclaw_tool_handler_t* h2_gizclaw_local_network_tools_internal(
    h2_gizclaw_local_t*, h2_gizclaw_service_t*);
void h2_gizclaw_local_network_response_copied_internal(
    h2_gizclaw_rpc_provider_response_t*);
bool h2_gizclaw_local_network_response_needs_drain_internal(
    const h2_gizclaw_rpc_provider_response_t*);
#endif
