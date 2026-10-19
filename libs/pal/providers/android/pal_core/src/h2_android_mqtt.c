#include "h2_android_mqtt.h"
#include "h2_android_net.h"
#include "h2_coremqtt.h"
#include "h2/pal/h2_pal_unsupported.h"
#include <string.h>
struct h2_android_mqtt {
    const h2_pal_mem_api_t *allocator;
    h2_android_net_t *network;
    h2_coremqtt_t *provider;
    h2_pal_mqtt_api_t api;
};
h2_pal_result_t h2_android_mqtt_create(const h2_pal_mem_api_t *allocator,
    h2_android_mqtt_t **out) {
    if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out = NULL;
    if (allocator == NULL) allocator = h2_android_platform_mem_api();
    if (allocator == NULL || allocator->vtable == NULL || allocator->vtable->alloc == NULL ||
        allocator->vtable->realloc == NULL || allocator->vtable->free == NULL) return H2_PAL_ERR_INVALID_ARG;
    h2_android_mqtt_t *owner = h2_pal_mem_alloc(allocator, sizeof(*owner));
    if (owner == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(owner, 0, sizeof(*owner)); owner->allocator = allocator;
    int rc = h2_android_net_create(&owner->network);
    if (rc == H2_PAL_OK) {
        h2_coremqtt_config_t config = {.allocator = allocator,
            .net = h2_android_net_api(owner->network), .time = h2_android_platform_time_api(),
            .outgoing_publish_records = 4u, .incoming_publish_records = 4u};
        rc = h2_coremqtt_create(&config, &owner->provider, &owner->api);
    }
    if (rc != H2_PAL_OK) {
        (void)h2_android_net_destroy(&owner->network);
        h2_pal_mem_free(allocator, owner); return rc;
    }
    *out = owner; return H2_PAL_OK;
}
const h2_pal_mqtt_api_t *h2_android_mqtt_api(h2_android_mqtt_t *owner) {
    return owner == NULL ? h2_pal_unsupported_mqtt_api() : &owner->api;
}
h2_pal_result_t h2_android_mqtt_destroy(h2_android_mqtt_t **out) {
    if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (*out == NULL) return H2_PAL_OK;
    h2_android_mqtt_t *owner = *out;
    int rc = h2_android_net_destroy(&owner->network);
    if (rc != H2_PAL_OK) return rc;
    h2_coremqtt_destroy(owner->provider);
    h2_pal_mem_free(owner->allocator, owner); *out = NULL; return H2_PAL_OK;
}
