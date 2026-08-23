#include "h2_corehttp.h"
#include "h2_coremqtt.h"
#include "h2_windows_platform.h"
#include "h2_wolfssl.h"

#include <assert.h>

int main(void) {
    const h2_windows_platform_config_t platform_config = {0};
    h2_windows_platform_t *platform = NULL;
    assert(h2_windows_platform_create(&platform_config, &platform) ==
           H2_PAL_OK);
    h2_wolfssl_config_t wolfssl_config = {
        .mem = *h2_windows_mem_api(platform),
        .entropy_user = platform,
        .entropy = h2_windows_entropy,
    };
    assert(h2_wolfssl_init(&wolfssl_config) == H2_PAL_OK);

    h2_corehttp_config_t http_config = {
        .allocator = h2_windows_mem_api(platform),
        .net = h2_windows_net_api(platform),
        .time = h2_windows_time_api(platform),
        .log = h2_windows_log_api(platform),
    };
    h2_corehttp_t *http = NULL;
    h2_pal_http_api_t http_api;
    assert(h2_corehttp_create(&http_config, &http, &http_api) == H2_PAL_OK);

    h2_coremqtt_config_t mqtt_config = {
        .allocator = h2_windows_mem_api(platform),
        .net = h2_windows_net_api(platform),
        .time = h2_windows_time_api(platform),
        .log = h2_windows_log_api(platform),
    };
    h2_coremqtt_t *mqtt = NULL;
    h2_pal_mqtt_api_t mqtt_api;
    assert(h2_coremqtt_create(&mqtt_config, &mqtt, &mqtt_api) == H2_PAL_OK);
    assert(http_api.vtable != NULL && mqtt_api.vtable != NULL);

    h2_coremqtt_destroy(mqtt);
    h2_corehttp_destroy(http);
    assert(h2_wolfssl_deinit() == H2_PAL_OK);
    assert(h2_windows_platform_destroy(&platform) == H2_PAL_OK);
    return 0;
}
