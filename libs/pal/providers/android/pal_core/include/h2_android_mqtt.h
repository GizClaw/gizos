#ifndef H2_ANDROID_MQTT_H
#define H2_ANDROID_MQTT_H
#include "h2_android_platform.h"
#ifdef __cplusplus
extern "C" {
#endif
/** @brief Own a real coreMQTT provider with native POSIX Net and verified TLS.
 * @param allocator Borrowed allocator, or NULL for platform Memory; alloc,
 * realloc and free must be present. Its lifetime covers create through destroy.
 * @param out Cleared on failure. The owner uses four incoming/outgoing QoS1
 * records; client config/TLS/span/network buffers remain borrowed until close.
 * The caller explicitly injects the API into Runtime; default AppHost is not
 * changed. Close all clients and quiesce operations before destroying owner. */
typedef struct h2_android_mqtt h2_android_mqtt_t;
h2_pal_result_t h2_android_mqtt_create(const h2_pal_mem_api_t *allocator,
    h2_android_mqtt_t **out);
/** @brief Borrow the MQTT API until destroy; NULL selects unsupported. */
const h2_pal_mqtt_api_t *h2_android_mqtt_api(h2_android_mqtt_t *owner);
/** @brief Release provider and TLS reference; clear owner only on success.
 * NULL *owner is allowed. A busy TLS owner is retained for retry. */
h2_pal_result_t h2_android_mqtt_destroy(h2_android_mqtt_t **owner);
#ifdef __cplusplus
}
#endif
#endif
