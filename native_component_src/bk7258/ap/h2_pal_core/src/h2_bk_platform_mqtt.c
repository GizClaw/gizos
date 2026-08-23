#include "h2_bk_platform_core.h"

#include "h2/pal/h2_pal_unsupported.h"

#if H2_BK_PLATFORM_CORE_ENABLE_MQTT
#include "h2_coremqtt.h"

static h2_coremqtt_t *s_bk_mqtt;
static h2_pal_mqtt_api_t s_bk_mqtt_api;
#endif

const h2_pal_mqtt_api_t *h2_bk_platform_mqtt_api(void) {
#if H2_BK_PLATFORM_CORE_ENABLE_MQTT
    if (s_bk_mqtt != NULL) {
        return &s_bk_mqtt_api;
    }

    h2_coremqtt_config_t config = {
        .allocator = h2_bk_platform_default_allocator(),
        .net = h2_bk_platform_net_api(),
        .time = h2_bk_platform_time_api(),
        .log = h2_bk_platform_log_api(),
        .outgoing_publish_records = 8u,
        .incoming_publish_records = 8u,
    };

    h2_coremqtt_t *mqtt = NULL;
    h2_pal_mqtt_api_t api = {0};
    h2_pal_result_t result = h2_coremqtt_create(&config, &mqtt, &api);
    if (result != H2_PAL_OK) {
        return NULL;
    }
    s_bk_mqtt_api = api;
    s_bk_mqtt = mqtt;
    return &s_bk_mqtt_api;
#else
    return h2_pal_unsupported_mqtt_api();
#endif
}
