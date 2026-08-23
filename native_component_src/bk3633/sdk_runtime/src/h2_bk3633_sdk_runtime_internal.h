#ifndef H2_BK3633_SDK_RUNTIME_INTERNAL_H
#define H2_BK3633_SDK_RUNTIME_INTERNAL_H

#include "h2_bk3633_sdk_runtime.h"

h2_pal_result_t h2_bk3633_sdk_runtime_configure_rom_environment(
    const h2_bk3633_sdk_runtime_config_t *config);

void h2_bk3633_sdk_runtime_set_event(uint32_t event);
void h2_bk3633_sdk_runtime_clear_event(uint32_t event);
bool h2_bk3633_sdk_runtime_nvds_oversize_is_missing(
    uint8_t tag, size_t stored_size, size_t requested_capacity);
const h2_pal_mem_api_t *h2_bk3633_sdk_runtime_nvds_mem_api(void);

#endif
