#include "h2_bk_application_host.h"
#include "FreeRTOS.h"
#include "task.h"
#include <os/os.h>

static int startup_context(h2_bk_application_host_t *provider) {
    return !rtos_is_in_interrupt_context() &&
           xTaskGetCurrentTaskHandle() != NULL &&
           (void *)xTaskGetCurrentTaskHandle() == provider->startup_task;
}
static h2_pal_result_t prepare(void *user, h2_runtime_config_t *out) {
    h2_bk_application_host_t *provider = user;
    if (!startup_context(provider))
        return H2_PAL_ERR_INVALID_STATE;
    return provider->config.startup(provider->config.user, out);
}
static h2_pal_result_t poll(void *user, int *out_stop) {
    h2_bk_application_host_t *provider = user;
    if (!startup_context(provider))
        return H2_PAL_ERR_INVALID_STATE;
    *out_stop = provider->config.should_stop != NULL &&
                provider->config.should_stop(provider->config.user);
    rtos_delay_milliseconds(1);
    return H2_PAL_OK;
}
static h2_pal_result_t release(void *user) {
    h2_bk_application_host_t *provider = user;
    if (!startup_context(provider))
        return H2_PAL_ERR_INVALID_STATE;
    return provider->config.shutdown(provider->config.user);
}
static h2_pal_result_t activate(void *user, h2_runtime_t *runtime) {
    return h2_application_host_sdk_activate(
        &((h2_bk_application_host_t *)user)->config, runtime);
}
static h2_pal_result_t deactivate(void *user) {
    return h2_application_host_sdk_deactivate(
        &((h2_bk_application_host_t *)user)->config);
}
static void notify(void *user, h2_pal_application_host_event_t event) {
    h2_application_host_sdk_notify(&((h2_bk_application_host_t *)user)->config,
                                   event);
}
static const h2_pal_application_host_vtable_t vtable = {.prepare = prepare,
                                                        .activate = activate,
                                                        .poll = poll,
                                                        .deactivate =
                                                            deactivate,
                                                        .release = release,
                                                        .notify = notify};

h2_pal_result_t
h2_bk_application_host_init(h2_bk_application_host_t *provider,
                            const h2_application_host_sdk_config_t *config) {
    if (provider == NULL || config == NULL || config->startup == NULL ||
        config->shutdown == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    if (rtos_is_in_interrupt_context() || xTaskGetCurrentTaskHandle() == NULL)
        return H2_PAL_ERR_INVALID_STATE;
    *provider = (h2_bk_application_host_t){
        .config = *config,
        .api = {.user = provider, .vtable = &vtable},
        .startup_task = (void *)xTaskGetCurrentTaskHandle()};
    return H2_PAL_OK;
}
