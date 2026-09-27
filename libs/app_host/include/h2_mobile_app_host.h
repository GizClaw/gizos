#ifndef H2_MOBILE_APP_HOST_H
#define H2_MOBILE_APP_HOST_H
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Native Core assembly. Callers may replace unrelated capabilities before
 * Runtime creation. App selection, UI and lifecycle remain with the caller. */
h2_runtime_config_t h2_ios_app_host_config(void);
h2_pal_result_t h2_ios_app_host_create(h2_runtime_t **out);
void h2_ios_app_host_destroy(h2_runtime_t *runtime);
h2_runtime_config_t h2_android_app_host_config(void);
h2_pal_result_t h2_android_app_host_create(h2_runtime_t **out);
void h2_android_app_host_destroy(h2_runtime_t *runtime);
#ifdef __cplusplus
}
#endif
#endif
