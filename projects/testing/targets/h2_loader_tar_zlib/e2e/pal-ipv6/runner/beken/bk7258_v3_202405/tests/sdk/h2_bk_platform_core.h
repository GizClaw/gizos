#ifndef H2_IPV6_LAUNCHER_TEST_CORE_H
#define H2_IPV6_LAUNCHER_TEST_CORE_H
#include "net/h2_pal_dtls.h"
#include "os/h2_pal_mem.h"
h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void);
const h2_pal_dtls_api_t* h2_bk_platform_dtls_api(void);
#endif
