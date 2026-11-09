#ifndef H2_GIZCLAW_E2E_AMOLED_OTA_H
#define H2_GIZCLAW_E2E_AMOLED_OTA_H
#include "h2_device_service.h"
/** Blocking, explicitly selected real hardware OTA acceptance lane. */
void h2_gizclaw_e2e_amoled_ota_run(h2_runtime_t* runtime,
                                   h2_device_app_commands_t* management);
#endif
