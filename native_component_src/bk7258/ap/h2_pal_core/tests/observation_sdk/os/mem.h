#ifndef H2_TEST_BK_OBSERVATION_MEM_H
#define H2_TEST_BK_OBSERVATION_MEM_H

#include <stddef.h>

void* os_malloc(size_t size);
void* os_realloc(void* ptr, size_t size);
void os_free(void* ptr);
void* psram_malloc(size_t size);
void* psram_realloc(void* ptr, size_t size);

#endif
