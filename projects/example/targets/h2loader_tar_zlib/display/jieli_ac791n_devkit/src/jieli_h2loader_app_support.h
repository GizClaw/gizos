#ifndef JIELI_H2LOADER_APP_SUPPORT_H
#define JIELI_H2LOADER_APP_SUPPORT_H

#include "h2_loader_app_client.h"

int h2_jieli_app_loader_config_init(
    h2_loader_app_client_config_t *out_config,
    h2_pal_fs_api_t *fs,
    const h2_pal_power_api_t *power,
    h2_loader_memory_stats_api_t memory_stats,
    uint32_t hardware_capabilities);

int h2_jieli_app_loader_confirm(
    const h2_loader_app_client_config_t *config);

int h2_jieli_app_loader_ble_start(
    const h2_loader_app_client_config_t *config);

#endif
