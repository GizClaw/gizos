#ifndef H2_STORAGE_HOST_CONFIG_H
#define H2_STORAGE_HOST_CONFIG_H
#include "h2_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
h2_runtime_config_t h2_storage_host_config(const h2_pal_mem_api_t *memory);
#ifdef __cplusplus
}
#endif
#endif
