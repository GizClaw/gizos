#ifndef H2_GIZCLAW_DEVICE_INTERNAL_H
#define H2_GIZCLAW_DEVICE_INTERNAL_H
#include "h2_gizclaw_service.h"
typedef struct h2_gizclaw_device h2_gizclaw_device_t;
h2_pal_result_t h2_gizclaw_device_init_internal(h2_gizclaw_service_t *service);
h2_pal_result_t h2_gizclaw_device_start_internal(h2_gizclaw_device_t *device);
/** Test support only. Re-point the product vtable and power PAL of a
 * quiescent device: no inbound RPC may be in flight and no action may be
 * accepted (BUSY otherwise). Readers are not synchronized beyond that. */
h2_pal_result_t h2_gizclaw_device_set_product_internal(
    h2_gizclaw_service_t *service, const h2_gizclaw_vtable_t *vtable,
    const h2_pal_power_api_t *power);
/** Test support only. True while an accepted action (sound, reboot, Wi-Fi,
 * firmware) is still reserved, i.e. a new one would be refused BUSY. The
 * reservation outlives the action's last PAL callback. */
bool h2_gizclaw_device_action_pending_internal(h2_gizclaw_service_t *service);
void h2_gizclaw_device_cancel_internal(h2_gizclaw_device_t *device);
h2_pal_result_t h2_gizclaw_device_stop_internal(h2_gizclaw_device_t *device);
void h2_gizclaw_device_destroy_internal(h2_gizclaw_device_t *device);
int h2_gizclaw_device_rpc_internal(
    void *user, h2_gizclaw_rpc_method_t method, h2_gizclaw_rpc_bytes_t request,
    h2_gizclaw_rpc_provider_response_t *response);
#endif
