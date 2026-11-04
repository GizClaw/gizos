#ifndef H2_TEST_AUDIO_PLATFORM_H
#define H2_TEST_AUDIO_PLATFORM_H
#include "h2_pal.h"
h2_pal_mem_api_t* h2_esp_platform_default_allocator(void);
const h2_pal_queue_api_t* h2_esp_platform_queue_api(void);
const h2_pal_sync_api_t* h2_esp_platform_sync_api(void);
#endif
