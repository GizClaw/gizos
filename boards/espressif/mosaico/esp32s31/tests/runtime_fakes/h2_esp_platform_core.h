#ifndef H2_TEST_RUNTIME_PLATFORM_H
#define H2_TEST_RUNTIME_PLATFORM_H
#include "h2_pal.h"
const h2_pal_mem_api_t* h2_esp_platform_external_allocator(void);
const h2_pal_io_stream_api_t* h2_esp_platform_io_stream_api(void);
const h2_pal_timer_api_t* h2_esp_platform_timer_api(void);
const h2_pal_firmware_info_api_t* h2_esp_platform_firmware_info_api(void);
const h2_pal_device_identity_api_t* h2_esp_platform_device_identity_api(void);
const h2_pal_net_api_t* h2_esp_platform_net_api(void);
const h2_pal_netif_api_t* h2_esp_platform_netif_api(void);
const h2_pal_wifi_csi_api_t* h2_esp_platform_wifi_csi(void);
#endif
