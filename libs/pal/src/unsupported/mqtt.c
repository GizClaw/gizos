#include "h2_pal.h"
#include <stddef.h>
#include <string.h>

static h2_pal_result_t unsupported_mqtt_open(void *p0, const h2_pal_mqtt_client_config_t *p1, h2_pal_mqtt_client_t **p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_mqtt_connect(void *p0, h2_pal_mqtt_client_t *p1) {
    (void)p0;
    (void)p1;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_mqtt_disconnect(void *p0, h2_pal_mqtt_client_t *p1, uint32_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_mqtt_publish(void *p0, h2_pal_mqtt_client_t *p1, const h2_pal_mqtt_publish_t *p2, uint16_t *p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_mqtt_subscribe(void *p0, h2_pal_mqtt_client_t *p1, const h2_pal_mqtt_subscribe_request_t *p2, uint16_t *p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_mqtt_unsubscribe(void *p0, h2_pal_mqtt_client_t *p1, const h2_pal_mqtt_unsubscribe_request_t *p2, uint16_t *p3) {
    (void)p0;
    (void)p1;
    (void)p2;
    (void)p3;
    return H2_PAL_ERR_UNSUPPORTED;
}

static h2_pal_result_t unsupported_mqtt_process(void *p0, h2_pal_mqtt_client_t *p1, uint32_t p2) {
    (void)p0;
    (void)p1;
    (void)p2;
    return H2_PAL_ERR_UNSUPPORTED;
}

static void unsupported_mqtt_close(void *p0, h2_pal_mqtt_client_t *p1) {
    (void)p0;
    (void)p1;
}

static const h2_pal_mqtt_vtable_t unsupported_mqtt_vtable = {
    .open = unsupported_mqtt_open,
    .connect = unsupported_mqtt_connect,
    .disconnect = unsupported_mqtt_disconnect,
    .publish = unsupported_mqtt_publish,
    .subscribe = unsupported_mqtt_subscribe,
    .unsubscribe = unsupported_mqtt_unsubscribe,
    .process = unsupported_mqtt_process,
    .close = unsupported_mqtt_close,
};
static const h2_pal_mqtt_api_t unsupported_mqtt_api = { .user = NULL, .vtable = &unsupported_mqtt_vtable };
const h2_pal_mqtt_api_t *h2_pal_unsupported_mqtt_api(void) { return &unsupported_mqtt_api; }
