#include "h2_esp_application_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int startup_context(h2_esp_application_host_t *provider) {
    return !xPortInIsrContext() && xTaskGetCurrentTaskHandle() != NULL &&
           (void *)xTaskGetCurrentTaskHandle() == provider->startup_task;
}
static h2_pal_result_t prepare(void *user, struct h2_runtime_config *out) {
    h2_esp_application_host_t *provider = user;
    if (!startup_context(provider))
        return H2_PAL_ERR_INVALID_STATE;
    return provider->platform.vtable->prepare(provider->platform.user, out);
}
static h2_pal_result_t poll(void *user, int *out_stop) {
    h2_esp_application_host_t *provider = user;
    if (!startup_context(provider))
        return H2_PAL_ERR_INVALID_STATE;
    h2_pal_result_t rc =
        provider->platform.vtable->poll(provider->platform.user, out_stop);
    if (rc == H2_PAL_OK)
        vTaskDelay(1);
    return rc;
}
static h2_pal_result_t release(void *user) {
    h2_esp_application_host_t *provider = user;
    if (!startup_context(provider))
        return H2_PAL_ERR_INVALID_STATE;
    return provider->platform.vtable->release(provider->platform.user);
}
static h2_pal_result_t activate(void *user, struct h2_runtime *runtime) {
    h2_esp_application_host_t *provider = user;
    return provider->platform.vtable->activate(provider->platform.user,
                                               runtime);
}
static h2_pal_result_t deactivate(void *user) {
    h2_esp_application_host_t *provider = user;
    return provider->platform.vtable->deactivate(provider->platform.user);
}
static void notify(void *user, h2_pal_application_host_event_t event) {
    h2_esp_application_host_t *provider = user;
    provider->platform.vtable->notify(provider->platform.user, event);
}
static const h2_pal_application_host_vtable_t vtable = {.prepare = prepare,
                                                        .activate = activate,
                                                        .poll = poll,
                                                        .deactivate =
                                                            deactivate,
                                                        .release = release,
                                                        .notify = notify};

h2_pal_result_t
h2_esp_application_host_init(h2_esp_application_host_t *provider,
                             const h2_pal_application_host_api_t *platform) {
    if (provider == NULL || platform == NULL || platform->vtable == NULL ||
        platform->vtable->prepare == NULL ||
        platform->vtable->activate == NULL || platform->vtable->poll == NULL ||
        platform->vtable->deactivate == NULL ||
        platform->vtable->release == NULL || platform->vtable->notify == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    if (xPortInIsrContext() || xTaskGetCurrentTaskHandle() == NULL)
        return H2_PAL_ERR_INVALID_STATE;
    *provider = (h2_esp_application_host_t){
        .platform = *platform,
        .api = {.user = provider, .vtable = &vtable},
        .startup_task = (void *)xTaskGetCurrentTaskHandle()};
    return H2_PAL_OK;
}
