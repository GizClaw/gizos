#ifndef H2_GIZCLAW_DEVICE_RPC_INTERNAL_H
#define H2_GIZCLAW_DEVICE_RPC_INTERNAL_H
#include "h2_gizclaw_device.h"

typedef struct h2_gizclaw_device_rpc h2_gizclaw_device_rpc_t;
h2_pal_result_t h2_gizclaw_device_rpc_create_internal(
    const h2_gizclaw_device_config_t *platform,
    const h2_gizclaw_config_t *registrations, h2_gizclaw_device_rpc_t **out);
h2_pal_result_t
h2_gizclaw_device_rpc_start_internal(h2_gizclaw_device_rpc_t *rpc);
h2_pal_result_t
h2_gizclaw_device_rpc_stop_internal(h2_gizclaw_device_rpc_t *rpc);
h2_pal_result_t
h2_gizclaw_device_rpc_destroy_internal(h2_gizclaw_device_rpc_t **rpc);
h2_gizclaw_device_rpc_t *
h2_gizclaw_device_rpc_internal(h2_gizclaw_device_t *device);
/* The network SDK already validated its outer tool envelope. Its provider
 * submits only the selected inner payload to the same owner as local calls. */
int h2_gizclaw_device_network_tool_internal(
    void *user, h2_gizclaw_tool_t tool, h2_gizclaw_rpc_bytes_t request,
    h2_gizclaw_rpc_provider_response_t *response);
int h2_gizclaw_device_network_mhs_internal(
    h2_gizclaw_device_t *device, h2_gizclaw_rpc_method_t method,
    h2_gizclaw_rpc_bytes_t request,
    h2_gizclaw_rpc_provider_response_t *response);
void h2_gizclaw_device_rpc_cancel_network_internal(
    h2_gizclaw_device_rpc_t *rpc);
const h2_gizclaw_tool_handler_t *
h2_gizclaw_device_rpc_network_tools_internal(h2_gizclaw_device_rpc_t *rpc,
                                             h2_gizclaw_device_t *device);
void h2_gizclaw_device_network_response_copied_internal(
    h2_gizclaw_rpc_provider_response_t *response);
bool h2_gizclaw_device_rpc_quiescent_internal(h2_gizclaw_device_rpc_t *rpc);
bool h2_gizclaw_device_network_admitting_internal(h2_gizclaw_device_t *device);
bool h2_gizclaw_device_network_response_needs_drain_internal(
    const h2_gizclaw_rpc_provider_response_t *response);
#endif
