#ifndef H2_IPV6_LAUNCHER_TEST_LOADER_H
#define H2_IPV6_LAUNCHER_TEST_LOADER_H
#include "h2_runtime.h"
#define H2_LOADER_CAPABILITY_UART 1u
#define H2_LOADER_CAPABILITY_WIFI 2u
int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
    h2_runtime_t* runtime, const char* name, uint32_t capabilities);
int h2_bk_h2loader_confirm_current_app(h2_runtime_t* runtime);
const h2_pal_power_api_t* h2_bk_h2loader_power_api(void);
#endif
