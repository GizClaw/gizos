#include "h2_application_host_sdk.h"

h2_pal_result_t h2_application_host_sdk_activate(void *user,
                                                 h2_runtime_t *runtime) {
    h2_application_host_sdk_config_t *config = user;
    return config->acquire_input != NULL
               ? config->acquire_input(config->user, runtime)
               : H2_PAL_OK;
}
h2_pal_result_t h2_application_host_sdk_deactivate(void *user) {
    h2_application_host_sdk_config_t *config = user;
    return config->release_input != NULL ? config->release_input(config->user)
                                         : H2_PAL_OK;
}
void h2_application_host_sdk_notify(void *user,
                                    h2_pal_application_host_event_t event) {
    h2_application_host_sdk_config_t *config = user;
    if (config->notify != NULL)
        config->notify(config->user, event);
}
