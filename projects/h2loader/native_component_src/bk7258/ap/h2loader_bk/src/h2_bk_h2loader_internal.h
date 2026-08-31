#ifndef H2_BK_H2LOADER_INTERNAL_H
#define H2_BK_H2LOADER_INTERNAL_H

#include "h2_loader_app_client.h"
#include "h2_runtime.h"

#include <stdint.h>

int h2_bk_h2loader_select_confirmed_boot_partition(uint32_t partition_id);
int h2_bk_h2loader_prepare_pending_app_rollback(void);
int h2_bk_h2loader_prepare_app_operation(h2_runtime_t *runtime);
int h2_bk_h2loader_current_app_identity(
    h2_runtime_t *runtime,
    const char *version,
    h2_loader_image_identity_t *out_identity);
const h2_pal_power_api_t *h2_bk_h2loader_app_power_api(
    const h2_pal_pref_api_t *pref);
int h2_bk_h2loader_init_app_client(
    h2_runtime_t *runtime,
    const char *active_name,
    uint32_t hardware_capabilities,
    h2_loader_app_client_t *client);

#endif
