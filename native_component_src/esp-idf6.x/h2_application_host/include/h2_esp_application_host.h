#ifndef H2_ESP_APPLICATION_HOST_H
#define H2_ESP_APPLICATION_HOST_H
#include "h2_application_host_sdk.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Initialize on the SDK/BSP startup task, then run shared Host on that task.
 * Caller owns storage, copied callbacks and context until successful cleanup;
 * retain this object and all contexts when the shared Host reports retained.
 * One init and one Host run per instance. No private board is selected. */
typedef struct h2_esp_application_host {
    h2_application_host_sdk_config_t config;
    h2_pal_application_host_api_t api;
    void *startup_task;
} h2_esp_application_host_t;
h2_pal_result_t
h2_esp_application_host_init(h2_esp_application_host_t *provider,
                             const h2_application_host_sdk_config_t *config);
#ifdef __cplusplus
}
#endif
#endif
