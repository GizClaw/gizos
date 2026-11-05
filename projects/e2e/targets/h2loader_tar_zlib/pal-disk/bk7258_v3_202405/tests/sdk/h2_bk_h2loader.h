#include "h2_runtime.h"
#define H2_LOADER_CAPABILITY_UART 1u
int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(h2_runtime_t*,
                                                            const char*,
                                                            uint32_t);
int h2_bk_h2loader_confirm_current_app(h2_runtime_t*);
const h2_pal_power_api_t* h2_bk_h2loader_power_api(void);
