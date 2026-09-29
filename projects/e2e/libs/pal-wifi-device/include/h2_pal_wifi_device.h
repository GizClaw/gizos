#ifndef H2_PAL_WIFI_DEVICE_H
#define H2_PAL_WIFI_DEVICE_H
#include "h2_pal_wifi_e2e.h"
#ifdef __cplusplus
extern "C" {
#endif
extern const char h2_wifi_device_runner_task_name[];
int h2_wifi_device_run(h2_runtime_t *runtime, const char *version, const char *board);
void h2_wifi_device_report(void);
int h2_wifi_fixture_run(h2_runtime_t *runtime);
#ifdef __cplusplus
}
#endif
#endif
