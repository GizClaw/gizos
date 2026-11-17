#ifndef H2_TEST_BK_OBSERVATION_OS_H
#define H2_TEST_BK_OBSERVATION_OS_H

#include <stddef.h>

#ifndef CONFIG_PSRAM_AS_SYS_MEMORY
#define CONFIG_PSRAM_AS_SYS_MEMORY 1
#endif

size_t rtos_get_total_heap_size(void);
size_t rtos_get_free_heap_size(void);
size_t rtos_get_minimum_free_heap_size(void);
size_t rtos_get_psram_total_heap_size(void);
size_t rtos_get_psram_free_heap_size(void);
size_t rtos_get_psram_minimum_free_heap_size(void);

#endif
