#ifndef H2_DEVICE_APP_PREFLIGHT_H
#define H2_DEVICE_APP_PREFLIGHT_H

#include "h2_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_device_app_preflight {
  h2_pal_mutex_t* operation_mutex;
  h2_pal_firmware_info_t firmware_info;
  uint32_t app_partition_id;
} h2_device_app_preflight_t;

int h2_device_app_preflight(const h2_runtime_config_t* runtime_config,
                            h2_device_app_preflight_t* out_preflight);

#ifdef __cplusplus
}
#endif

#endif
