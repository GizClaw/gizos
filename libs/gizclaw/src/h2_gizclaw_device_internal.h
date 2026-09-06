#ifndef H2_GIZCLAW_DEVICE_INTERNAL_H
#define H2_GIZCLAW_DEVICE_INTERNAL_H
#include "h2_gizclaw_service.h"
typedef struct h2_gizclaw_device h2_gizclaw_device_t;
h2_pal_result_t h2_gizclaw_device_init_internal(h2_gizclaw_service_t *service);
h2_pal_result_t h2_gizclaw_device_start_internal(h2_gizclaw_device_t *device);
void h2_gizclaw_device_cancel_internal(h2_gizclaw_device_t *device);
h2_pal_result_t h2_gizclaw_device_stop_internal(h2_gizclaw_device_t *device);
void h2_gizclaw_device_destroy_internal(h2_gizclaw_device_t *device);
int h2_gizclaw_device_rpc_internal(
    void *user, h2_gizclaw_rpc_method_t method, h2_gizclaw_rpc_bytes_t request,
    h2_gizclaw_rpc_provider_response_t *response);
#endif
